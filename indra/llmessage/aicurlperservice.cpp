/**
 * @file aiperservice.cpp
 * @brief Implementation of AIPerService
 *
 * Copyright (c) 2012, 2013, Aleric Inglewood.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution.
 *
 * CHANGELOG
 *   and additional copyright holders.
 *
 *   04/11/2012
 *   Initial version, written by Aleric Inglewood @ SL
 *
 *   06/04/2013
 *   Renamed AICurlPrivate::PerHostRequestQueue[Ptr] to AIPerHostRequestQueue[Ptr]
 *   to allow public access.
 *
 *   09/04/2013
 *   Renamed everything "host" to "service" and use "hostname:port" as key
 *   instead of just "hostname".
 */

#include "sys.h"
#include "aicurlperservice.h"
#include "aicurlthread.h"
#include "llcontrol.h"

AIPerService::threadsafe_instance_map_type AIPerService::sInstanceMap;
AIThreadSafeSimpleDC<AIPerService::TotalQueued> AIPerService::sTotalQueued;

#undef AICurlPrivate

namespace AICurlPrivate {

// Cached value of CurlConcurrentConnectionsPerService.
U32 CurlConcurrentConnectionsPerService;

// Friend functions of RefCountedThreadSafePerService

void intrusive_ptr_add_ref(RefCountedThreadSafePerService* per_service)
{
  per_service->mReferenceCount++;
}

void intrusive_ptr_release(RefCountedThreadSafePerService* per_service)
{
  if (--per_service->mReferenceCount == 0)
  {
    delete per_service;
  }
}

} // namespace AICurlPrivate

using namespace AICurlPrivate;

AIPerService::AIPerService(void) :
		mHTTPBandwidth(25),	// 25 = 1000 ms / 40 ms.
		mConcurrectConnections(CurlConcurrentConnectionsPerService),
		mTotalAdded(0),
		mApprovedFirst(0),
		mUnapprovedFirst(0)
{
}

AIPerService::CapabilityType::CapabilityType(void) :
  		mApprovedRequests(0),
		mQueuedCommands(0),
		mAdded(0),
		mFlags(0),
		mDownloading(0),
		mMaxPipelinedRequests(CurlConcurrentConnectionsPerService)
{
}

AIPerService::CapabilityType::~CapabilityType()
{
}

// Fake copy constructor.
AIPerService::AIPerService(AIPerService const&) : mHTTPBandwidth(0)
{
}

// url must be of the form
// (see http://www.ietf.org/rfc/rfc3986.txt Appendix A for definitions not given here):
//
// url			= sheme ":" hier-part [ "?" query ] [ "#" fragment ]
// hier-part	= "//" authority path-abempty
// authority     = [ userinfo "@" ] host [ ":" port ]
// path-abempty  = *( "/" segment )
//
// That is, a hier-part of the form '/ path-absolute', '/ path-rootless' or
// '/ path-empty' is NOT allowed here. This should be safe because we only
// call this function for curl access, any file access would use APR.
//
// However, as a special exception, this function allows:
//
// url			= authority path-abempty
//
// without the 'sheme ":" "//"' parts.
//
// As follows from the ABNF (see RFC, Appendix A):
// - authority is either terminated by a '/' or by the end of the string because
//   neither userinfo, host nor port may contain a '/'.
// - userinfo does not contain a '@', and if it exists, is always terminated by a '@'.
// - port does not contain a ':', and if it exists is always prepended by a ':'.
//
//static
std::string AIPerService::extract_canonical_servicename(std::string const& url)
{
  char const* p = url.data();
  char const* const end = p + url.size();
  char const* sheme_colon = NULL;
  char const* sheme_slash = NULL;
  char const* first_ampersand = NULL;
  char const* port_colon = NULL;
  std::string servicename;
  char const* hostname = p;                 // Default in the case there is no "sheme://userinfo@".
  while (p < end)
  {
    int c = *p;
    if (c == ':')
    {
      if (!port_colon && LLStringOps::isDigit(p[1]))
      {
        port_colon = p;
      }
      else if (!sheme_colon && !sheme_slash && !first_ampersand && !port_colon)
      {
        // Found a colon before any slash or ampersand: this has to be the colon between the sheme and the hier-part.
        sheme_colon = p;
      }
    }
    else if (c == '/')
    {
      if (!sheme_slash && sheme_colon && sheme_colon == p - 1 && !first_ampersand && p[1] == '/')
      {
        // Found the first '/' in the first occurance of the sequence "://".
        sheme_slash = p;
        hostname = ++p + 1;                 // Point hostname to the start of the authority, the default when there is no "userinfo@" part.
        servicename.clear();                // Remove the sheme.
      }
      else
      {
        // Found slash that is not part of the "sheme://" string. Signals end of authority.
        // We're done.
        break;
      }
    }
    else if (c == '@')
    {
      if (!first_ampersand)
      {
        first_ampersand = p;
        hostname = p + 1;
        servicename.clear();                // Remove the "userinfo@"
      }
    }
    if (p >= hostname)
    {
      // Convert hostname to lowercase in a way that we compare two hostnames equal iff libcurl does.
#if APR_CHARSET_EBCDIC
#error Not implemented
#else
      if (c >= 'A' && c <= 'Z')
        c += ('a' - 'A');
#endif
      servicename += c;
    }
    ++p;
  }
  // Strip of any trailing ":80".
  if (p - 3 == port_colon && p[-1] == '0' && p[-2] == '8')
  {
    return servicename.substr(0, p - hostname - 3);
  }
  return servicename;
}

//static
AIPerServicePtr AIPerService::instance(std::string const& servicename)
{
  llassert(!servicename.empty());
  instance_map_wat instance_map_w(sInstanceMap);
  AIPerService::iterator iter = instance_map_w->find(servicename);
  if (iter == instance_map_w->end())
  {
	iter = instance_map_w->insert(instance_map_type::value_type(servicename, new RefCountedThreadSafePerService)).first;
  }
  // Note: the creation of AIPerServicePtr MUST be protected by the lock on sInstanceMap (see release()).
  return iter->second;
}

//static
void AIPerService::release(AIPerServicePtr& instance)
{
  if (instance->exactly_two_left())		// Being 'instance' and the one in sInstanceMap.
  {
	// The viewer can be have left main() we can't access the global sInstanceMap anymore.
	if (LLApp::isStopped())
	{
	  return;
	}
	instance_map_wat instance_map_w(sInstanceMap);
	// It is possible that 'exactly_two_left' is not up to date anymore.
	// Therefore, recheck the condition now that we have locked sInstanceMap.
	if (!instance->exactly_two_left())
	{
	  // Some other thread added this service in the meantime.
	  return;
	}
#ifdef SHOW_ASSERT
	{
	  // The reference in the map is the last one; that means there can't be any curl easy requests queued for this service.
	  PerService_rat per_service_r(*instance);
	  for (int i = 0; i < number_of_capability_types; ++i)
	  {
	  	llassert(per_service_r->mCapabilityType[i].mQueuedRequests.empty());
	  }
	}
#endif
	// Find the service and erase it from the map.
	iterator const end = instance_map_w->end();
	for(iterator iter = instance_map_w->begin(); iter != end; ++iter)
	{
	  if (instance == iter->second)
	  {
		instance_map_w->erase(iter);
		instance.reset();
		return;
	  }
	}
	// We should always find the service.
	llassert(false);
  }
  instance.reset();
}

bool AIPerService::throttled() const
{
  return mTotalAdded >= mConcurrectConnections;
}

void AIPerService::added_to_multi_handle(AICapabilityType capability_type)
{
  ++mCapabilityType[capability_type].mAdded;
  ++mTotalAdded;
}

void AIPerService::removed_from_multi_handle(AICapabilityType capability_type, bool downloaded_something)
{
  llassert(mTotalAdded > 0 && mCapabilityType[capability_type].mAdded > 0);
  --mCapabilityType[capability_type].mAdded;
  if (downloaded_something)
  {
	llassert(mCapabilityType[capability_type].mDownloading > 0);
	--mCapabilityType[capability_type].mDownloading;
  }
  --mTotalAdded;
}

void AIPerService::queue(AICurlEasyRequest const& easy_request, AICapabilityType capability_type)
{
  mCapabilityType[capability_type].mQueuedRequests.push_back(easy_request.get_ptr());
  TotalQueued_wat(sTotalQueued)->count++;
}

bool AIPerService::cancel(AICurlEasyRequest const& easy_request, AICapabilityType capability_type)
{
  CapabilityType::queued_request_type::iterator const end = mCapabilityType[capability_type].mQueuedRequests.end();
  CapabilityType::queued_request_type::iterator cur = std::find(mCapabilityType[capability_type].mQueuedRequests.begin(), end, easy_request.get_ptr());

  if (cur == end)
	return false;		// Not found.

  // We can't use erase because that uses assignment to move elements,
  // because it isn't thread-safe. Therefore, move the element that we found to 
  // the back with swap (could just swap with the end immediately, but I don't
  // want to break the order in which requests where added). Swap is also not
  // thread-safe, but OK here because it only touches the objects in the deque,
  // and the deque is protected by the lock on the AIPerService object.
  CapabilityType::queued_request_type::iterator prev = cur;
  while (++cur != end)
  {
	prev->swap(*cur);				// This is safe,
	prev = cur;
  }
  mCapabilityType[capability_type].mQueuedRequests.pop_back();		// if this is safe.
  TotalQueued_wat total_queued_w(sTotalQueued);
  total_queued_w->count--;
  llassert(total_queued_w->count >= 0);
  return true;
}

void AIPerService::add_queued_to(curlthread::MultiHandle* multi_handle, bool recursive)
{
  int order[number_of_capability_types];
  // The first two types are approved types, they should be the first to try.
  // Try the one that has the largest queue first, if they the queues have equal size, try mApprovedFirst first.
  size_t s0 = mCapabilityType[0].mQueuedRequests.size();
  size_t s1 = mCapabilityType[1].mQueuedRequests.size();
  if (s0 == s1)
  {
	order[0] = mApprovedFirst;
	mApprovedFirst = 1 - mApprovedFirst;
	order[1] = mApprovedFirst;
  }
  else if (s0 > s1)
  {
	order[0] = 0;
	order[1] = 1;
  }
  else
  {
	order[0] = 1;
	order[1] = 0;
  }
  // The next two types are unapproved types. Here, try them alternating regardless of queue size.
  int n = mUnapprovedFirst;
  for (int i = 2; i < number_of_capability_types; ++i, n = (n + 1) % (number_of_capability_types - 2))
  {
	order[i] = 2 + n;
  }
  mUnapprovedFirst = (mUnapprovedFirst + 1) % (number_of_capability_types - 2);

  for (int i = 0; i < number_of_capability_types; ++i)
  {
	CapabilityType& ct(mCapabilityType[order[i]]);
	if (!ct.mQueuedRequests.empty())
	{
	  if (!multi_handle->add_easy_request(ct.mQueuedRequests.front(), true))
	  {
		// Throttled. If this failed then every capability type will fail: we either are using too much bandwidth, or too many total connections.
		// However, it MAY be that this service was thottled for using too much bandwidth by itself. Look if other services can be added.
		break;
	  }
	  // Request was added, remove it from the queue.
	  ct.mQueuedRequests.pop_front();
	  if (ct.mQueuedRequests.empty())
	  {
		// We obtained a request from the queue, and after that there we no more request in the queue of this service.
		ct.mFlags |= ctf_empty;
	  }
	  else
	  {
		// We obtained a request from the queue, and even after that there was at least one more request in the queue of this service.
		ct.mFlags |= ctf_full;
	  }
	  TotalQueued_wat total_queued_w(sTotalQueued);
	  llassert(total_queued_w->count > 0);
	  if (!--(total_queued_w->count))
	  {
		// We obtained a request from the queue, and after that there we no more request in any queue.
		total_queued_w->empty = true;
	  }
	  else
	  {
		// We obtained a request from the queue, and even after that there was at least one more request in some queue.
		total_queued_w->full = true;
	  }
	  // We added something from a queue, so we're done.
	  return;
	}
	else
	{
	  // We could add a new request, but there is none in the queue!
	  // Note that if this service does not serve this capability type,
	  // then obviously this queue was empty; however, in that case
	  // this variable will never be looked at, so it's ok to set it.
	  ct.mFlags |= ctf_starvation;
	}
	if (i == number_of_capability_types - 1)
	{
	  // Last entry also empty. All queues of this service were empty. Check total connections.
	  TotalQueued_wat total_queued_w(sTotalQueued);
	  if (total_queued_w->count == 0)
	  {
		// The queue of every service is empty!
		total_queued_w->starvation = true;
		return;
	  }
	}
  }
  if (recursive)
  {
	return;
  }
  // Nothing from this service could be added, try other services.
  instance_map_wat instance_map_w(sInstanceMap);
  for (iterator service = instance_map_w->begin(); service != instance_map_w->end(); ++service)
  {
	PerService_wat per_service_w(*service->second);
	if (&*per_service_w == this)
	{
	  continue;
	}
	per_service_w->add_queued_to(multi_handle, true);
  }
}

//static
void AIPerService::purge(void)
{
  instance_map_wat instance_map_w(sInstanceMap);
  for (iterator service = instance_map_w->begin(); service != instance_map_w->end(); ++service)
  {
	Dout(dc::curl, "Purging queues of service \"" << service->first << "\".");
	PerService_wat per_service_w(*service->second);
	TotalQueued_wat total_queued_w(sTotalQueued);
	for (int i = 0; i < number_of_capability_types; ++i)
	{
	  size_t s = per_service_w->mCapabilityType[i].mQueuedRequests.size();
	  per_service_w->mCapabilityType[i].mQueuedRequests.clear();
	  total_queued_w->count -= s;
	  llassert(total_queued_w->count >= 0);
	}
  }
}

//static
void AIPerService::adjust_concurrent_connections(int increment)
{
  instance_map_wat instance_map_w(sInstanceMap);
  for (AIPerService::iterator iter = instance_map_w->begin(); iter != instance_map_w->end(); ++iter)
  {
	PerService_wat per_service_w(*iter->second);
	U32 old_concurrent_connections = per_service_w->mConcurrectConnections;
	per_service_w->mConcurrectConnections = llclamp(old_concurrent_connections + increment, (U32)1, CurlConcurrentConnectionsPerService);
	increment = per_service_w->mConcurrectConnections - old_concurrent_connections;
	for (int i = 0; i < number_of_capability_types; ++i)
	{
	  per_service_w->mCapabilityType[i].mMaxPipelinedRequests = llmax(per_service_w->mCapabilityType[i].mMaxPipelinedRequests + increment, (U32)0);
	}
  }
}

void AIPerService::Approvement::honored(void)
{
  if (!mHonored)
  {
	mHonored = true;
	PerService_wat per_service_w(*mPerServicePtr);
	llassert(per_service_w->mCapabilityType[mCapabilityType].mApprovedRequests > 0);
	per_service_w->mCapabilityType[mCapabilityType].mApprovedRequests--;
  }
}

void AIPerService::Approvement::not_honored(void)
{
  honored();
  llwarns << "Approvement for has not been honored." << llendl;
}

