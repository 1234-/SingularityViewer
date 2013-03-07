/**
 * @file lltexturefetch.cpp
 * @brief Object which fetches textures from the cache and/or network
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include <iostream>

#include <boost/regex.hpp>
#include <boost/lexical_cast.hpp>

#include "llstl.h"
#include "message.h"

#include "lltexturefetch.h"

#include "llcurl.h"
#include "lldir.h"
#include "llhttpclient.h"
#include "llhttpstatuscodes.h"
#include "llimage.h"
#include "llimagej2c.h"
#include "llimageworker.h"
#include "llworkerthread.h"
#include "message.h"

#include "llagent.h"
#include "lltexturecache.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"
#include "llviewertexture.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llviewerstatsrecorder.h"
#include "llworld.h"
#include "llstartup.h"
#include "llbuffer.h"

class AIHTTPTimeoutPolicy;
extern AIHTTPTimeoutPolicy HTTPGetResponder_timeout;
extern AIHTTPTimeoutPolicy lcl_responder_timeout;

LLStat LLTextureFetch::sCacheHitRate("texture_cache_hits", 128);
LLStat LLTextureFetch::sCacheReadLatency("texture_cache_read_latency", 128);

//////////////////////////////////////////////////////////////////////////////
class LLTextureFetchWorker : public LLWorkerClass
{
	friend class LLTextureFetch;
	friend class HTTPGetResponder;
	
private:
	class CacheReadResponder : public LLTextureCache::ReadResponder
	{
	public:
		CacheReadResponder(LLTextureFetch* fetcher, const LLUUID& id, LLImageFormatted* image)
			: mFetcher(fetcher), mID(id)
		{
			setImage(image);
		}
		virtual void completed(bool success)
		{
			LLTextureFetchWorker* worker = mFetcher->getWorker(mID);
			if (worker)
			{
 				worker->callbackCacheRead(success, mFormattedImage, mImageSize, mImageLocal);
			}
		}
	private:
		LLTextureFetch* mFetcher;
		LLUUID mID;
	};

	class CacheWriteResponder : public LLTextureCache::WriteResponder
	{
	public:
		CacheWriteResponder(LLTextureFetch* fetcher, const LLUUID& id)
			: mFetcher(fetcher), mID(id)
		{
		}
		virtual void completed(bool success)
		{
			LLTextureFetchWorker* worker = mFetcher->getWorker(mID);
			if (worker)
			{
				worker->callbackCacheWrite(success);
			}
		}
	private:
		LLTextureFetch* mFetcher;
		LLUUID mID;
	};
	
	class DecodeResponder : public LLImageDecodeThread::Responder
	{
	public:
		DecodeResponder(LLTextureFetch* fetcher, const LLUUID& id, LLTextureFetchWorker* worker)
			: mFetcher(fetcher), mID(id), mWorker(worker)
		{
		}
		virtual void completed(bool success, LLImageRaw* raw, LLImageRaw* aux)
		{
			LLTextureFetchWorker* worker = mFetcher->getWorker(mID);
			if (worker)
			{
 				worker->callbackDecoded(success, raw, aux);
			}
		}
	private:
		LLTextureFetch* mFetcher;
		LLUUID mID;
		LLTextureFetchWorker* mWorker; // debug only (may get deleted from under us, use mFetcher/mID)
	};

	struct Compare
	{
		// lhs < rhs
		bool operator()(const LLTextureFetchWorker* lhs, const LLTextureFetchWorker* rhs) const
		{
			// greater priority is "less"
			const F32 lpriority = lhs->mImagePriority;
			const F32 rpriority = rhs->mImagePriority;
			if (lpriority > rpriority) // higher priority
				return true;
			else if (lpriority < rpriority)
				return false;
			else
				return lhs < rhs;
		}
	};
	
public:
	/*virtual*/ bool doWork(S32 param); // Called from LLWorkerThread::processRequest()
	/*virtual*/ void finishWork(S32 param, bool completed); // called from finishRequest() (WORK THREAD)
	/*virtual*/ bool deleteOK(); // called from update() (WORK THREAD)

	~LLTextureFetchWorker();
	// void relese() { --mActiveCount; }

	S32 callbackHttpGet(const LLChannelDescriptors& channels,
						 const LLHTTPClient::ResponderBase::buffer_ptr_t& buffer,
						 bool partial, bool success);
	void callbackCacheRead(bool success, LLImageFormatted* image,
						   S32 imagesize, BOOL islocal);
	void callbackCacheWrite(bool success);
	void callbackDecoded(bool success, LLImageRaw* raw, LLImageRaw* aux);
	
	void setGetStatus(U32 status, const std::string& reason)
	{
		LLMutexLock lock(&mWorkMutex);

		mGetStatus = status;
		mGetReason = reason;
	}

	void setCanUseHTTP(bool can_use_http) { mCanUseHTTP = can_use_http; }
	bool getCanUseHTTP() const { return mCanUseHTTP; }

	LLTextureFetch & getFetcher() { return *mFetcher; }
	
protected:
	LLTextureFetchWorker(LLTextureFetch* fetcher, const std::string& url, const LLUUID& id, const LLHost& host,
						 F32 priority, S32 discard, S32 size);

private:
	/*virtual*/ void startWork(S32 param); // called from addWork() (MAIN THREAD)
	/*virtual*/ void endWork(S32 param, bool aborted); // called from doWork() (MAIN THREAD)

	void resetFormattedData();
	
	void setImagePriority(F32 priority);
	void setDesiredDiscard(S32 discard, S32 size);
	bool insertPacket(S32 index, U8* data, S32 size);
	void clearPackets();
	void setupPacketData();
	U32 calcWorkPriority();
	void removeFromCache();
	bool processSimulatorPackets();
	bool writeToCacheComplete();
	
	void lockWorkMutex() { mWorkMutex.lock(); }
	void unlockWorkMutex() { mWorkMutex.unlock(); }

private:
	enum e_state // mState
	{
		// NOTE: Affects LLTextureBar::draw in lltextureview.cpp (debug hack)
		INVALID = 0,
		INIT,
		LOAD_FROM_TEXTURE_CACHE,
		CACHE_POST,
		LOAD_FROM_NETWORK,
		LOAD_FROM_SIMULATOR,
		SEND_HTTP_REQ,
		WAIT_HTTP_REQ,
		DECODE_IMAGE,
		DECODE_IMAGE_UPDATE,
		WRITE_TO_CACHE,
		WAIT_ON_WRITE,
		DONE
	};
	enum e_request_state // mSentRequest
	{
		UNSENT = 0,
		QUEUED = 1,
		SENT_SIM = 2
	};
	enum e_write_to_cache_state //mWriteToCacheState
	{
		NOT_WRITE = 0,
		CAN_WRITE = 1,
		SHOULD_WRITE = 2
	};
	static const char* sStateDescs[];
	e_state mState;
	e_write_to_cache_state mWriteToCacheState;
	LLTextureFetch* mFetcher;
	LLPointer<LLImageFormatted> mFormattedImage;
	LLPointer<LLImageRaw> mRawImage;
	LLPointer<LLImageRaw> mAuxImage;
	LLUUID mID;
	LLHost mHost;
	std::string mUrl;
	U8 mType;
	F32 mImagePriority;
	U32 mWorkPriority;
	F32 mRequestedPriority;
	S32 mDesiredDiscard;
	S32 mSimRequestedDiscard;
	S32 mRequestedDiscard;
	S32 mLoadedDiscard;
	S32 mDecodedDiscard;
	LLFrameTimer mRequestedTimer;
	LLFrameTimer mFetchTimer;
	LLTimer			mCacheReadTimer;
	F32				mCacheReadTime;
	LLTextureCache::handle_t mCacheReadHandle;
	LLTextureCache::handle_t mCacheWriteHandle;
	std::vector<U8> mHttpBuffer;
	S32 mRequestedSize;
	S32 mRequestedOffset;
	S32 mDesiredSize;
	S32 mFileSize;
	S32 mCachedSize;	
	e_request_state mSentRequest;
	handle_t mDecodeHandle;
	BOOL mLoaded;
	BOOL mDecoded;
	BOOL mWritten;
	BOOL mNeedsAux;
	BOOL mHaveAllData;
	BOOL mInLocalCache;
	bool mCanUseHTTP ;
	bool mCanUseNET ; //can get from asset server.
	S32 mHTTPFailCount;
	S32 mRetryAttempt;
	S32 mActiveCount;
	U32 mGetStatus;
	std::string mGetReason;
	
	// Work Data
	LLMutex mWorkMutex;
	struct PacketData
	{
		PacketData(U8* data, S32 size) { mData = data; mSize = size; }
		~PacketData() { clearData(); }
		void clearData() { delete[] mData; mData = NULL; }
		U8* mData;
		U32 mSize;
	};
	std::vector<PacketData*> mPackets;
	S32 mFirstPacket;
	S32 mLastPacket;
	U16 mTotalPackets;
	U8 mImageCodec;
};

//////////////////////////////////////////////////////////////////////////////
class HTTPGetResponder : public LLHTTPClient::ResponderWithCompleted
{
	LOG_CLASS(HTTPGetResponder);
public:
	HTTPGetResponder(LLTextureFetch* fetcher, const LLUUID& id, U64 startTime, S32 requestedSize, U32 offset, bool redir)
		: mFetcher(fetcher)
		, mID(id)
		, mStartTime(startTime)
		, mRequestedSize(requestedSize)
		, mRequestedOffset(offset)
		, mFollowRedir(redir)
	{
	}
	~HTTPGetResponder()
	{
	}

#if 0 //Apparently, SL never sends content-range and instead sends transfer-encoding: chunked, so disabling for now
	/*virtual*/ bool needsHeaders(void) const { return true; }

	/*virtual*/ void completedHeaders(U32 status, std::string const& reason, AIHTTPReceivedHeaders const& headers) {
		llinfos << "Texture fetch HTTP status: " << status << llendl;
		llinfos << "Texture fetch headers: " << headers << llendl;
		//example: Content-Range: 1000-3979/3980  Content-Length: 2980
		static const boost::regex pattern("\\w*bytes\\w+(\\d+)-(\\d+)/(\\d+)");
		std::string	rangehdr;
		if (headers.getFirstValue("content-range", rangehdr)){
			llinfos << "Have content-range header" <<llendl;
			boost::smatch match;
			llassert_always(boost::regex_match(rangehdr, match, pattern));
			llassert(match.length() == 3);

			std::string lengthhdr;
			if(headers.getFirstValue("content-length",lengthhdr)) {
				//insert length checking
			}
		}
	}
#endif

	/*virtual*/ void completedRaw(U32 status, const std::string& reason,
							  const LLChannelDescriptors& channels,
							  const buffer_ptr_t& buffer)
	{
		static LLCachedControl<bool> log_to_viewer_log(gSavedSettings,"LogTextureDownloadsToViewerLog");
		static LLCachedControl<bool> log_to_sim(gSavedSettings,"LogTextureDownloadsToSimulator");
		static LLCachedControl<bool> log_texture_traffic(gSavedSettings,"LogTextureNetworkTraffic") ;

		if (log_to_viewer_log || log_to_sim)
		{
			mFetcher->mTextureInfo.setRequestStartTime(mID, mStartTime);
			U64 timeNow = LLTimer::getTotalTime();
			mFetcher->mTextureInfo.setRequestType(mID, LLTextureInfoDetails::REQUEST_TYPE_HTTP);
			mFetcher->mTextureInfo.setRequestSize(mID, mRequestedSize);
			mFetcher->mTextureInfo.setRequestOffset(mID, mRequestedOffset);
			mFetcher->mTextureInfo.setRequestCompleteTimeAndLog(mID, timeNow);
		}

		LL_DEBUGS("Texture") << "HTTP COMPLETE: " << mID << LL_ENDL;
		LLTextureFetchWorker* worker = mFetcher->getWorker(mID);
		if (worker)
		{
			bool success = false;
			bool partial = false;
			if (HTTP_OK <= status &&  status < HTTP_MULTIPLE_CHOICES)
			{
				success = true;
				if (HTTP_PARTIAL_CONTENT == status) // partial information
				{
					partial = true;
				}
			}
			if (!success)
			{
				worker->setGetStatus(status, reason);
// 				llwarns << "CURL GET FAILED, status:" << status << " reason:" << reason << llendl;
			}
			S32 data_size = worker->callbackHttpGet(channels, buffer, partial, success);
			
			if(log_texture_traffic && data_size > 0)
			{
				LLViewerTexture* tex = LLViewerTextureManager::findTexture(mID) ;
				if(tex)
				{
					gTotalTextureBytesPerBoostLevel[tex->getBoostLevel()] += data_size ;
				}
			}

			mFetcher->removeFromHTTPQueue(mID, data_size);
		}
		else
		{
			mFetcher->removeFromHTTPQueue(mID);
 			llwarns << "Worker not found: " << mID << llendl;
		}
	}
	
	/*virtual*/ bool followRedir() const
	{
		return mFollowRedir;
	}
	
	/*virtual*/ AIHTTPTimeoutPolicy const& getHTTPTimeoutPolicy(void) const { return HTTPGetResponder_timeout; }
	/*virtual*/ char const* getName(void) const { return "HTTPGetResponder"; }

private:
	LLTextureFetch* mFetcher;
	LLUUID mID;
	U64 mStartTime;
	S32 mRequestedSize;
	U32 mRequestedOffset;
	bool mFollowRedir;
};

//////////////////////////////////////////////////////////////////////////////

class SGHostBlackList{
	static const int MAX_ERRORCOUNT = 20;
	struct BlackListEntry {
		std::string host;
		U64 timeUntil;
		U32 reason;
		int errorCount;
	};

	typedef std::vector<BlackListEntry> blacklist_t;
	//Why is it a vector? because using std::map for just 1-2 values is insane-ish.
	typedef blacklist_t::iterator iter;
	static blacklist_t blacklist;

	static bool is_obsolete(BlackListEntry entry) {
			U64 now = LLTimer::getTotalTime();
			return (now > entry.timeUntil);
	} //should make a functor. if i cared.

	static void cleanup() {
		std::remove_if(blacklist.begin(), blacklist.end(), is_obsolete);
	}

	static iter find(std::string host) {
		cleanup();
		for(blacklist_t::iterator i = blacklist.begin(); i != blacklist.end(); ++i) {
			if (i->host.find(host) == 0) return i;
		}
		return blacklist.end();
	}

public:
	static bool isBlacklisted(std::string url) {
		iter found = find(url);
		bool r = (found != blacklist.end()) && (found->errorCount > MAX_ERRORCOUNT);
		return r;
	}

	static void add(std::string url, float timeout, U32 reason) {
		llwarns << "Requested adding to blacklist: " << url << llendl;
		BlackListEntry entry;
		entry.host = url.substr(0, url.rfind("/"));
		if (entry.host.empty()) return;
		entry.timeUntil = LLTimer::getTotalTime() + timeout*1000;
		entry.reason = reason;
		entry.errorCount = 0;
		iter found = find(entry.host);
		if(found != blacklist.end()) {
			entry.errorCount = found->errorCount + 1;
			*found = entry;
			if (entry.errorCount > MAX_ERRORCOUNT) {
				std::string s;
				microsecondsToTimecodeString(entry.timeUntil, s);
				llwarns << "Blacklisting address " << entry.host
					<< "is blacklisted for " << timeout 
					<< " seconds because of error " << reason
					<< llendl;
			}
		}
		else blacklist.push_back(entry);
	}
};

SGHostBlackList::blacklist_t SGHostBlackList::blacklist;

#if 0
//call every time a connection is opened
//return true if connecting allowed
static bool sgConnectionThrottle() {
	const U32 THROTTLE_TIMESTEPS_PER_SECOND = 10;
	static const LLCachedControl<U32> max_connections_per_second("HTTPRequestRate", 30);
	U32 max_connections = max_connections_per_second/THROTTLE_TIMESTEPS_PER_SECOND;
	const U32 timestep = USEC_PER_SEC/THROTTLE_TIMESTEPS_PER_SECOND;
	U64 now = LLTimer::getTotalTime();
	std::deque<U64> timestamps;
	while(!timestamps.empty() && (timestamps[0]<=now-timestep)) {
		timestamps.pop_front();
	}
	if(timestamps.size() < max_connections) {
		//llinfos << "throttle pass" << llendl;
		timestamps.push_back(now);
		return true;
	} else {
		//llinfos << "throttle fail" << llendl;
		return false;
	}
}
#endif

//////////////////////////////////////////////////////////////////////////////

//static
const char* LLTextureFetchWorker::sStateDescs[] = {
	"INVALID",
	"INIT",
	"LOAD_FROM_TEXTURE_CACHE",
	"CACHE_POST",
	"LOAD_FROM_NETWORK",
	"LOAD_FROM_SIMULATOR",
	"SEND_HTTP_REQ",
	"WAIT_HTTP_REQ",
	"DECODE_IMAGE",
	"DECODE_IMAGE_UPDATE",
	"WRITE_TO_CACHE",
	"WAIT_ON_WRITE",
	"DONE",
};

// called from MAIN THREAD

LLTextureFetchWorker::LLTextureFetchWorker(LLTextureFetch* fetcher,
										   const std::string& url, // Optional URL
										   const LLUUID& id,	// Image UUID
										   const LLHost& host,	// Simulator host
										   F32 priority,		// Priority
										   S32 discard,			// Desired discard
										   S32 size)			// Desired size
	: LLWorkerClass(fetcher, "TextureFetch"),
	  mState(INIT),
	  mWriteToCacheState(NOT_WRITE),
	  mFetcher(fetcher),
	  mID(id),
	  mHost(host),
	  mUrl(url),
	  mImagePriority(priority),
	  mWorkPriority(0),
	  mRequestedPriority(0.f),
	  mDesiredDiscard(-1),
	  mSimRequestedDiscard(-1),
	  mRequestedDiscard(-1),
	  mLoadedDiscard(-1),
	  mDecodedDiscard(-1),
	  mCacheReadTime(0.f),
	  mCacheReadHandle(LLTextureCache::nullHandle()),
	  mCacheWriteHandle(LLTextureCache::nullHandle()),
	  mRequestedSize(0),
	  mRequestedOffset(0),
	  mDesiredSize(TEXTURE_CACHE_ENTRY_SIZE),
	  mFileSize(0),
	  mCachedSize(0),
	  mLoaded(FALSE),
	  mSentRequest(UNSENT),
	  mDecodeHandle(0),
	  mDecoded(FALSE),
	  mWritten(FALSE),
	  mNeedsAux(FALSE),
	  mHaveAllData(FALSE),
	  mInLocalCache(FALSE),
	  mCanUseHTTP(true),
	  mHTTPFailCount(0),
	  mRetryAttempt(0),
	  mActiveCount(0),
	  mGetStatus(0),
	  mFirstPacket(0),
	  mLastPacket(-1),
	  mTotalPackets(0),
	  mImageCodec(IMG_CODEC_INVALID)
{
	mCanUseNET = mUrl.empty() ;

	calcWorkPriority();
	mType = host.isOk() ? LLImageBase::TYPE_AVATAR_BAKE : LLImageBase::TYPE_NORMAL;
 	//llinfos << "Create: " << mID << " mHost:" << host << " Discard=" << discard << " URL:"<< mUrl << llendl;
	if (!mFetcher->mDebugPause)
	{
		U32 work_priority = mWorkPriority | LLWorkerThread::PRIORITY_HIGH;
		addWork(0, work_priority );
	}
	setDesiredDiscard(discard, size);
}

LLTextureFetchWorker::~LLTextureFetchWorker()
{
// 	llinfos << "Destroy: " << mID
// 			<< " Decoded=" << mDecodedDiscard
// 			<< " Requested=" << mRequestedDiscard
// 			<< " Desired=" << mDesiredDiscard << llendl;
	llassert_always(!haveWork());
	lockWorkMutex();
	if (mCacheReadHandle != LLTextureCache::nullHandle() && mFetcher->mTextureCache)
	{
		mFetcher->mTextureCache->readComplete(mCacheReadHandle, true);
	}
	if (mCacheWriteHandle != LLTextureCache::nullHandle() && mFetcher->mTextureCache)
	{
		mFetcher->mTextureCache->writeComplete(mCacheWriteHandle, true);
	}
	mFormattedImage = NULL;
	clearPackets();
	unlockWorkMutex();
	mFetcher->removeFromHTTPQueue(mID);
}

void LLTextureFetchWorker::clearPackets()
{
	for_each(mPackets.begin(), mPackets.end(), DeletePointer());
	mPackets.clear();
	mTotalPackets = 0;
	mLastPacket = -1;
	mFirstPacket = 0;
}

void LLTextureFetchWorker::setupPacketData()
{
	S32 data_size = 0;
	if (mFormattedImage.notNull())
	{
		data_size = mFormattedImage->getDataSize();
	}
	if (data_size > 0)
	{
		// Only used for simulator requests
		mFirstPacket = (data_size - FIRST_PACKET_SIZE) / MAX_IMG_PACKET_SIZE + 1;
		if (FIRST_PACKET_SIZE + (mFirstPacket-1) * MAX_IMG_PACKET_SIZE != data_size)
		{
			llwarns << "Bad CACHED TEXTURE size: " << data_size << " removing." << llendl;
			removeFromCache();
			resetFormattedData();
			clearPackets();
		}
		else if (mFileSize > 0)
		{
			mLastPacket = mFirstPacket-1;
			mTotalPackets = (mFileSize - FIRST_PACKET_SIZE + MAX_IMG_PACKET_SIZE-1) / MAX_IMG_PACKET_SIZE + 1;
		}
		else
		{
			// This file was cached using HTTP so we have to refetch the first packet
			resetFormattedData();
			clearPackets();
		}
	}
}

U32 LLTextureFetchWorker::calcWorkPriority()
{
// 	llassert_always(mImagePriority >= 0 && mImagePriority <= LLViewerTexture::maxDecodePriority());
	static const F32 PRIORITY_SCALE = (F32)LLWorkerThread::PRIORITY_LOWBITS / LLViewerFetchedTexture::maxDecodePriority();
	mWorkPriority = llmin((U32)LLWorkerThread::PRIORITY_LOWBITS, (U32)(mImagePriority * PRIORITY_SCALE));
	return mWorkPriority;
}

// mWorkMutex is locked
void LLTextureFetchWorker::setDesiredDiscard(S32 discard, S32 size)
{
	bool prioritize = false;
	if (mDesiredDiscard != discard)
	{
		if (!haveWork())
		{
			calcWorkPriority();
			if (!mFetcher->mDebugPause)
			{
				U32 work_priority = mWorkPriority | LLWorkerThread::PRIORITY_HIGH;
				addWork(0, work_priority);
			}
		}
		else if (mDesiredDiscard < discard)
		{
			prioritize = true;
		}
		mDesiredDiscard = discard;
		mDesiredSize = size;
	}
	else if (size > mDesiredSize)
	{
		mDesiredSize = size;
		prioritize = true;
	}
	mDesiredSize = llmax(mDesiredSize, TEXTURE_CACHE_ENTRY_SIZE);
	if ((prioritize && mState == INIT) || mState == DONE)
	{
		mState = INIT;
		U32 work_priority = mWorkPriority | LLWorkerThread::PRIORITY_HIGH;
		setPriority(work_priority);
	}
}

void LLTextureFetchWorker::setImagePriority(F32 priority)
{
// 	llassert_always(priority >= 0 && priority <= LLViewerTexture::maxDecodePriority());
	F32 delta = fabs(priority - mImagePriority);
	if (delta > (mImagePriority * .05f) || mState == DONE)
	{
		mImagePriority = priority;
		calcWorkPriority();
		U32 work_priority = mWorkPriority | (getPriority() & LLWorkerThread::PRIORITY_HIGHBITS);
		setPriority(work_priority);
	}
}

void LLTextureFetchWorker::resetFormattedData()
{
	std::vector<U8>().swap(mHttpBuffer);
	if (mFormattedImage.notNull())
	{
		mFormattedImage->deleteData();
	}
	mHaveAllData = FALSE;
}

// Called from MAIN thread
void LLTextureFetchWorker::startWork(S32 param)
{
	llassert(mFormattedImage.isNull());
}

#include "llviewertexturelist.h" // debug

// Called from LLWorkerThread::processRequest()
bool LLTextureFetchWorker::doWork(S32 param)
{
	static const F32 FETCHING_TIMEOUT = 15.f;//seconds

	LLMutexLock lock(&mWorkMutex);

	if ((mFetcher->isQuitting() || getFlags(LLWorkerClass::WCF_DELETE_REQUESTED)))
	{
		if (mState < DECODE_IMAGE)
	{
			return true; // abort
		}
	}
	if(mImagePriority < F_ALMOST_ZERO)
	{
		if (mState == INIT || mState == LOAD_FROM_NETWORK || mState == LOAD_FROM_SIMULATOR)
		{
			return true; // abort
		}
	}
	if(mState > CACHE_POST && !mCanUseNET && !mCanUseHTTP)
	{
		//nowhere to get data, abort.
		return true ;
	}
	
	if (mFetcher->mDebugPause)
	{
		return false; // debug: don't do any work
	}
	if (mID == mFetcher->mDebugID)
	{
		mFetcher->mDebugCount++; // for setting breakpoints
	}

	if (mState != DONE)
	{
		mFetchTimer.reset();
	}

	if (mState == INIT)
	{
		if(gAssetStorage && std::find(gAssetStorage->mBlackListedAsset.begin(),
			gAssetStorage->mBlackListedAsset.end(),mID) != gAssetStorage->mBlackListedAsset.end())
		{
			llinfos << "Blacklisted asset " << mID.asString() << " was trying to be accessed!!!!!!" << llendl;
			mState = DONE;
			return true;
		}

		mRawImage = NULL ;
		mRequestedDiscard = -1;
		mLoadedDiscard = -1;
		mDecodedDiscard = -1;
		mRequestedSize = 0;
		mRequestedOffset = 0;
		mFileSize = 0;
		mCachedSize = 0;
		mLoaded = FALSE;
		mSentRequest = UNSENT;
		mDecoded  = FALSE;
		mWritten  = FALSE;
		std::vector<U8>().swap(mHttpBuffer);
		mHaveAllData = FALSE;
		clearPackets(); // TODO: Shouldn't be necessary
		mCacheReadHandle = LLTextureCache::nullHandle();
		mCacheWriteHandle = LLTextureCache::nullHandle();
		mState = LOAD_FROM_TEXTURE_CACHE;
		mDesiredSize = llmax(mDesiredSize, TEXTURE_CACHE_ENTRY_SIZE); // min desired size is TEXTURE_CACHE_ENTRY_SIZE
		LL_DEBUGS("Texture") << mID << ": Priority: " << llformat("%8.0f",mImagePriority)
							 << " Desired Discard: " << mDesiredDiscard << " Desired Size: " << mDesiredSize << LL_ENDL;
		// fall through
	}

	if (mState == LOAD_FROM_TEXTURE_CACHE)
	{
		if (mCacheReadHandle == LLTextureCache::nullHandle())
		{
			U32 cache_priority = mWorkPriority;
			S32 offset = mFormattedImage.notNull() ? mFormattedImage->getDataSize() : 0;
			S32 size = mDesiredSize - offset;
			if (size <= 0)
			{
				mState = CACHE_POST;
				return false;
			}
			mFileSize = 0;
			mLoaded = FALSE;

			if (mUrl.compare(0, 7, "file://") == 0)
			{
				setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority); // Set priority first since Responder may change it

				// read file from local disk
				std::string filename = mUrl.substr(7, std::string::npos);
				CacheReadResponder* responder = new CacheReadResponder(mFetcher, mID, mFormattedImage);
				mCacheReadHandle = mFetcher->mTextureCache->readFromCache(filename, mID, cache_priority,
																		  offset, size, responder);
				mCacheReadTimer.reset();
			}
			else if (mUrl.empty())
			{
				setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority); // Set priority first since Responder may change it

				CacheReadResponder* responder = new CacheReadResponder(mFetcher, mID, mFormattedImage);
				mCacheReadHandle = mFetcher->mTextureCache->readFromCache(mID, cache_priority,
																		  offset, size, responder);
				mCacheReadTimer.reset();
			}
			else if(!mUrl.empty() && mCanUseHTTP)
			{
				if (!(mUrl.compare(0, 7, "http://") == 0))
				{
					// *TODO:?remove this warning
					llwarns << "Unknown URL Type: " << mUrl << llendl;
				}
				setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
				mState = SEND_HTTP_REQ;
			}
			else
			{
				setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
				mState = LOAD_FROM_NETWORK;
			}
		}

		if (mLoaded)
		{
			// Make sure request is complete. *TODO: make this auto-complete
			if (mFetcher->mTextureCache->readComplete(mCacheReadHandle, false))
			{
				mCacheReadHandle = LLTextureCache::nullHandle();
				mState = CACHE_POST;
				// fall through
			}
			else
			{
				//
				//This should never happen
				//
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	if (mState == CACHE_POST)
	{
		mCachedSize = mFormattedImage.notNull() ? mFormattedImage->getDataSize() : 0;
		// Successfully loaded
		if ((mCachedSize >= mDesiredSize) || mHaveAllData)
		{
			// we have enough data, decode it
			llassert_always(mFormattedImage->getDataSize() > 0);
			mLoadedDiscard = mDesiredDiscard;
			mState = DECODE_IMAGE;
			mWriteToCacheState = NOT_WRITE ;
			LL_DEBUGS("Texture") << mID << ": Cached. Bytes: " << mFormattedImage->getDataSize()
								 << " Size: " << llformat("%dx%d",mFormattedImage->getWidth(),mFormattedImage->getHeight())
								 << " Desired Discard: " << mDesiredDiscard << " Desired Size: " << mDesiredSize << LL_ENDL;
			LLTextureFetch::sCacheHitRate.addValue(100.f);
		}
		else
		{
			if (mUrl.compare(0, 7, "file://") == 0)
			{
				// failed to load local file, we're done.
				return true;
			}
			// need more data
			else
			{
				LL_DEBUGS("Texture") << mID << ": Not in Cache" << LL_ENDL;
				mState = LOAD_FROM_NETWORK;
			}
			
			// fall through
			LLTextureFetch::sCacheHitRate.addValue(0.f);
		}
	}

	if (mState == LOAD_FROM_NETWORK)
	{
		static LLCachedControl<bool> use_http(gSavedSettings,"ImagePipelineUseHTTP");

// 		if (mHost != LLHost::invalid) use_http = false;
		if (use_http && mCanUseHTTP && mUrl.empty())	// get http url.
		{
			LLViewerRegion* region = NULL;
			if (mHost == LLHost::invalid)
				region = gAgent.getRegion();
			else
				region = LLWorld::getInstance()->getRegion(mHost);

			if (region)
			{
				std::string http_url = region->getHttpUrl() ;
				if (!http_url.empty())
				{
					mUrl = http_url + "/?texture_id=" + mID.asString().c_str();
					mWriteToCacheState = CAN_WRITE ; //because this texture has a fixed texture id.
				}
				else
				{
					mCanUseHTTP = false ;
				}
			}
			else
			{
				// This will happen if not logged in or if a region deoes not have HTTP Texture enabled
				//llwarns << "Region not found for host: " << mHost << llendl;
				mCanUseHTTP = false;
			}
		}
		if (!mUrl.empty() && SGHostBlackList::isBlacklisted(mUrl)){
			mCanUseHTTP = false;
		}
		if (mCanUseHTTP && !mUrl.empty())
		{
			mState = LLTextureFetchWorker::SEND_HTTP_REQ;
			setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
			if(mWriteToCacheState != NOT_WRITE)
			{
				mWriteToCacheState = CAN_WRITE ;
			}
			// don't return, fall through to next state
		}
		else if (mSentRequest == UNSENT && mCanUseNET)
		{
			// Add this to the network queue and sit here.
			// LLTextureFetch::update() will send off a request which will change our state
			mWriteToCacheState = CAN_WRITE ;
			mRequestedSize = mDesiredSize;
			mRequestedDiscard = mDesiredDiscard;
			mSentRequest = QUEUED;
			mFetcher->addToNetworkQueue(this);
			setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority);
			
			return false;
		}
		else
		{
			// Shouldn't need to do anything here
			//llassert_always(mFetcher->mNetworkQueue.find(mID) != mFetcher->mNetworkQueue.end());
			// Make certain this is in the network queue
			//mFetcher->addToNetworkQueue(this);
			//setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority);
			return false;
		}
	}
	
	if (mState == LOAD_FROM_SIMULATOR)
	{
		if (mFormattedImage.isNull())
		{
			mFormattedImage = new LLImageJ2C;
		}
		if (processSimulatorPackets())
		{
			LL_DEBUGS("Texture") << mID << ": Loaded from Sim. Bytes: " << mFormattedImage->getDataSize() << LL_ENDL;
			mFetcher->removeFromNetworkQueue(this, false);
			if (mFormattedImage.isNull() || !mFormattedImage->getDataSize())
			{
				// processSimulatorPackets() failed
// 				llwarns << "processSimulatorPackets() failed to load buffer" << llendl;
				return true; // failed
			}
			setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
			mState = DECODE_IMAGE;
			mWriteToCacheState = SHOULD_WRITE;
		}
		else
		{
			mFetcher->addToNetworkQueue(this); // failsafe
			setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority);
		}
		return false;
	}
	
	if (mState == SEND_HTTP_REQ)
	{
		if(mCanUseHTTP)
		{
			//NOTE:
			//control the number of the http requests issued for:
			//1, not opening too many file descriptors at the same time;
			//2, control the traffic of http so udp gets bandwidth.
			//
			static const LLCachedControl<U32> max_http_requests("HTTPMaxRequests", 8);
			static const LLCachedControl<U32> min_http_requests("HTTPMinRequests", 2);
			static const LLCachedControl<F32> throttle_bandwidth("HTTPThrottleBandwidth", 2000);
			if(((U32)mFetcher->getNumHTTPRequests() >= max_http_requests) ||
			   ((mFetcher->getTextureBandwidth() > throttle_bandwidth) &&
				((U32)mFetcher->getNumHTTPRequests() > min_http_requests)))
			{
				return false ; //wait.
			}

			mFetcher->removeFromNetworkQueue(this, false);
			
			S32 cur_size = 0;
			if (mFormattedImage.notNull())
			{
				cur_size = mFormattedImage->getDataSize(); // amount of data we already have
				if (mFormattedImage->getDiscardLevel() == 0)
				{
					if(cur_size > 0)
					{
						// We already have all the data, just decode it
						mLoadedDiscard = mFormattedImage->getDiscardLevel();
						mState = DECODE_IMAGE;
						return false;
					}
					else
					{
						return true ; //abort.
					}
				}
			}
			mRequestedSize = mDesiredSize - cur_size;
			mRequestedDiscard = mDesiredDiscard;
			mRequestedOffset = cur_size;
			
			bool res = false;
			if (!mUrl.empty())
			{
				mRequestedTimer.reset();

				mLoaded = FALSE;
				mGetStatus = 0;
				mGetReason.clear();
				static const LLCachedControl<F32> throttle_bandwidth("HTTPThrottleBandwidth", 2000);
				LL_DEBUGS("Texture") << "HTTP GET: " << mID << " Offset: " << mRequestedOffset
									 << " Bytes: " << mRequestedSize
									 << " Bandwidth(kbps): " << mFetcher->getTextureBandwidth() << "/" << throttle_bandwidth
									 << LL_ENDL;
				setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority);
				mState = WAIT_HTTP_REQ;	

				mFetcher->addToHTTPQueue(mID);

				if(mRequestedOffset>0)
				{
					// Texture fetching often issues 'speculative' loads that
					// start beyond the end of the actual asset.  Some cache/web
					// systems, e.g. Varnish, will respond to this not with a
					// 416 but with a 200 and the entire asset in the response
					// body.  By ensuring that we always have a partially
					// satisfiable Range request, we avoid that hit to the network.
					// We just have to deal with the overlapping data which is made
					// somewhat harder by the fact that grid services don't necessarily
					// return the Content-Range header on 206 responses.  *Sigh*
					mRequestedSize++;
					mRequestedOffset--;
				}

				  // Will call callbackHttpGet when curl request completes
				  AIHTTPHeaders headers("Accept", "image/x-j2c");
				  // Call LLHTTPClient::request directly instead of LLHTTPClient::getByteRange, because we want to pass a NULL AIEngine.
				  if (mRequestedOffset > 0 || mRequestedSize > 0)
				  {
					  headers.addHeader("Range", llformat("bytes=%d-%d", mRequestedOffset, mRequestedOffset + mRequestedSize - 1));
				  }
				  LLHTTPClient::request(mUrl, LLHTTPClient::HTTP_GET, NULL,
					  new HTTPGetResponder(mFetcher, mID, LLTimer::getTotalTime(), mRequestedSize, mRequestedOffset, true),
					  headers/*,*/ DEBUG_CURLIO_PARAM(false), keep_alive, false, false, NULL, 0, NULL);
				  res = true;
			}
			if (!res)
			{
				llwarns << "HTTP GET request failed for " << mID << llendl;
				resetFormattedData();
				++mHTTPFailCount;
				return true; // failed
			}
			// fall through
		}
		else //can not use http fetch.
		{
			return true ; //abort
		}
	}
	
	if (mState == WAIT_HTTP_REQ)
	{
		if (mLoaded)
		{
			S32 cur_size = mFormattedImage.notNull() ? mFormattedImage->getDataSize() : 0;
			if (mRequestedSize < 0)
			{
				S32 max_attempts;
				if (mGetStatus == HTTP_NOT_FOUND || mGetStatus == 499)
				{
					mHTTPFailCount = max_attempts = 1; // Don't retry
					if(mGetStatus == HTTP_NOT_FOUND)
						llwarns << "Texture missing from server (404): " << mUrl << llendl;
					else if (mGetStatus == 499) 
					{
						llwarns << "No response from server (499): " << mUrl << llendl;
						SGHostBlackList::add(mUrl, 60.0, mGetStatus);
					}
					//roll back to try UDP
					if(mCanUseNET)
					{
						resetFormattedData();
						mState = INIT ;
						mCanUseHTTP = false ;
						setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
						return false ;
					}
					else
					{
						// UDP is not an option, we are dead
						resetFormattedData();
						return true; // failed
					}
				}
				else if (mGetStatus == HTTP_SERVICE_UNAVAILABLE)
				{
					// *TODO: Should probably introduce a timer here to delay future HTTP requsts
					// for a short time (~1s) to ease server load? Ideally the server would queue
					// requests instead of returning 503... we already limit the number pending.
					++mHTTPFailCount;
					max_attempts = mHTTPFailCount+1; // Keep retrying
					LL_INFOS_ONCE("Texture") << "Texture server busy (503): " << mUrl << LL_ENDL;
				}
				else
				{
					const S32 HTTP_MAX_RETRY_COUNT = 3;
					max_attempts = HTTP_MAX_RETRY_COUNT + 1;
					++mHTTPFailCount;
					llinfos << "HTTP GET failed for: " << mUrl
							<< " Status: " << mGetStatus << " Reason: '" << mGetReason << "'"
							<< " Attempt:" << mHTTPFailCount+1 << "/" << max_attempts << llendl;
				}

				if (mHTTPFailCount >= max_attempts)
				{
					// Make max_attempts attempt at decoding what data we have,
                    // then bail forever on this image
					if (cur_size > 0 && (mHTTPFailCount < (max_attempts+1)) )
					{
						// Use available data
						mLoadedDiscard = mFormattedImage->getDiscardLevel();
						mState = DECODE_IMAGE;
						return false; 
					}
						else
						{
    						//roll back to try UDP
    						if(mCanUseNET)
    						{
								resetFormattedData();
    							mState = INIT ;
    							mCanUseHTTP = false ;
    							setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
    							return false ;
    						}
    						else
    						{
    							// UDP is not an option, we are dead
    							resetFormattedData();
                                mState = DONE;
    							return true; // failed
    						}
						}
					}
				else
				{
					mState = SEND_HTTP_REQ;
					return false; // retry
				}
			}

			if(mHttpBuffer.empty())//no data received.
			{
				//abort.
				mState = DONE;
				return true;
			}

			S32 total_size(cur_size + mRequestedSize);
			S32 src_offset(0);

			if(mRequestedOffset && mRequestedOffset != cur_size)
			{
				// In case of a partial response, our offset may
				// not be trivially contiguous with the data we have.
				// Get back into alignment.
				if (mRequestedOffset > cur_size)
				{
					LL_WARNS("Texture") << "Partial HTTP response produces break in image data for texture "
										<< mID << ".  Aborting load."  << LL_ENDL;
					mState = DONE;
					return true;
				}
				src_offset = cur_size - mRequestedOffset;
				total_size -= src_offset;
				mRequestedSize -= src_offset;                   // Make requested values reflect useful part
				mRequestedOffset += src_offset;

			}
			llassert(total_size == cur_size + mRequestedSize);

			if (mFormattedImage.isNull())
			{
				// For now, create formatted image based on extension
				std::string extension = gDirUtilp->getExtension(mUrl);
				mFormattedImage = LLImageFormatted::createFromType(LLImageBase::getCodecFromExtension(extension));
				if (mFormattedImage.isNull())
				{
					mFormattedImage = new LLImageJ2C; // default
				}
			}
			
			if (mHaveAllData && mRequestedDiscard == 0) //the image file is fully loaded.
			{
				mFileSize = total_size;
			}
			else //the file size is unknown.
			{
				mFileSize = total_size + 1 ; //flag the file is not fully loaded.
			}
			
			U8* buffer = (U8*)ALLOCATE_MEM(LLImageBase::getPrivatePool(), total_size);
			if (cur_size > 0)
			{
				memcpy(buffer, mFormattedImage->getData(), cur_size);
			}
			if (mRequestedSize > 0)
			{
				memcpy(buffer + mRequestedOffset, &mHttpBuffer[src_offset], mRequestedSize); // append
			}
			// NOTE: setData releases current data and owns new data (buffer)
			mFormattedImage->setData(buffer, total_size);
			// delete temp data
			std::vector<U8>().swap(mHttpBuffer);
			mLoadedDiscard = mRequestedDiscard;
			mState = DECODE_IMAGE;
			if(mWriteToCacheState != NOT_WRITE)
			{
				mWriteToCacheState = SHOULD_WRITE ;
			}
			setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
			return false;
		}
		else
		{
			if(FETCHING_TIMEOUT < mRequestedTimer.getElapsedTimeF32())
			{
				//timeout, abort.
				mState = DONE;
				return true;
			}

			setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority);
			return false;
		}
	}
	
	if (mState == DECODE_IMAGE)
	{
		static LLCachedControl<bool> textures_decode_disabled(gSavedSettings,"TextureDecodeDisabled");

		setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority); // Set priority first since Responder may change it
		if (textures_decode_disabled)
		{
			// for debug use, don't decode
			mState = DONE;
			return true;
		}

		if (mDesiredDiscard < 0)
		{
			// We aborted, don't decode
			mState = DONE;
			return true;
		}
		
		if (mFormattedImage->getDataSize() <= 0)
		{
			//llerrs << "Decode entered with invalid mFormattedImage. ID = " << mID << llendl;
			
			//abort, don't decode
			mState = DONE;
			return true;
		}
		if (mLoadedDiscard < 0)
		{
			//llerrs << "Decode entered with invalid mLoadedDiscard. ID = " << mID << llendl;

			//abort, don't decode
			mState = DONE;
			return true;
		}

		mRawImage = NULL;
		mAuxImage = NULL;
		llassert_always(mFormattedImage.notNull());
		S32 discard = mHaveAllData ? 0 : mLoadedDiscard;
		U32 image_priority = LLWorkerThread::PRIORITY_NORMAL | mWorkPriority;
		mDecoded  = FALSE;
		mState = DECODE_IMAGE_UPDATE;
		LL_DEBUGS("Texture") << mID << ": Decoding. Bytes: " << mFormattedImage->getDataSize() << " Discard: " << discard
				<< " All Data: " << mHaveAllData << LL_ENDL;
		mDecodeHandle = mFetcher->mImageDecodeThread->decodeImage(mFormattedImage, image_priority, discard, mNeedsAux,
																  new DecodeResponder(mFetcher, mID, this));
		// fall though
	}
	
	if (mState == DECODE_IMAGE_UPDATE)
	{
		if (mDecoded)
		{
			if (mDecodedDiscard < 0)
			{
				LL_DEBUGS("Texture") << mID << ": Failed to Decode." << LL_ENDL;
				if (mCachedSize > 0 && !mInLocalCache && mRetryAttempt == 0)
				{
					// Cache file should be deleted, try again
// 					llwarns << mID << ": Decode of cached file failed (removed), retrying" << llendl;
					llassert_always(mDecodeHandle == 0);
					mFormattedImage = NULL;
					++mRetryAttempt;
					setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
					mState = INIT;
					return false;
				}
				else
				{
// 					llwarns << "UNABLE TO LOAD TEXTURE: " << mID << " RETRIES: " << mRetryAttempt << llendl;
					mState = DONE; // failed
				}
			}
			else
			{
				llassert_always(mRawImage.notNull());
				LL_DEBUGS("Texture") << mID << ": Decoded. Discard: " << mDecodedDiscard
						<< " Raw Image: " << llformat("%dx%d",mRawImage->getWidth(),mRawImage->getHeight()) << LL_ENDL;
				setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
				mState = WRITE_TO_CACHE;
			}
			// fall through
		}
		else
		{
			return false;
		}
	}

	if (mState == WRITE_TO_CACHE)
	{
		if (mWriteToCacheState != SHOULD_WRITE || mFormattedImage.isNull())
		{
			// If we're in a local cache or we didn't actually receive any new data,
			// or we failed to load anything, skip
			mState = DONE;
			return false;
		}
		S32 datasize = mFormattedImage->getDataSize();
		if (mFileSize < datasize)	// This could happen when http fetching and sim fetching mixed.
		{
			if (mHaveAllData)
			{
				mFileSize = datasize;
			}
			else
			{
				mFileSize = datasize + 1; // flag not fully loaded.
			}
		}
		llassert_always(datasize);
		setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority); // Set priority first since Responder may change it
		U32 cache_priority = mWorkPriority;
		mWritten = FALSE;
		mState = WAIT_ON_WRITE;
		CacheWriteResponder* responder = new CacheWriteResponder(mFetcher, mID);
		mCacheWriteHandle = mFetcher->mTextureCache->writeToCache(mID, cache_priority,
																  mFormattedImage->getData(), datasize,
																  mFileSize, responder);
		// fall through
	}
	
	if (mState == WAIT_ON_WRITE)
	{
		if (writeToCacheComplete())
		{
			mState = DONE;
			// fall through
		}
		else
		{
			if (mDesiredDiscard < mDecodedDiscard)
			{
				// We're waiting for this write to complete before we can receive more data
				// (we can't touch mFormattedImage until the write completes)
				// Prioritize the write
				mFetcher->mTextureCache->prioritizeWrite(mCacheWriteHandle);
			}
			return false;
		}
	}

	if (mState == DONE)
	{
		if (mDecodedDiscard >= 0 && mDesiredDiscard < mDecodedDiscard)
		{
			// More data was requested, return to INIT
			mState = INIT;
			setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
			return false;
		}
		else
		{
			setPriority(LLWorkerThread::PRIORITY_LOW | mWorkPriority);
			return true;
		}
	}
	
	return false;
}

// Called from MAIN thread
void LLTextureFetchWorker::endWork(S32 param, bool aborted)
{
	if (mDecodeHandle != 0)
	{
		mFetcher->mImageDecodeThread->abortRequest(mDecodeHandle, false);
		mDecodeHandle = 0;
	}
	mFormattedImage = NULL;
}

//////////////////////////////////////////////////////////////////////////////

// virtual
void LLTextureFetchWorker::finishWork(S32 param, bool completed)
{
	// The following are required in case the work was aborted
	if (mCacheReadHandle != LLTextureCache::nullHandle())
	{
		mFetcher->mTextureCache->readComplete(mCacheReadHandle, true);
		mCacheReadHandle = LLTextureCache::nullHandle();
	}
	if (mCacheWriteHandle != LLTextureCache::nullHandle())
	{
		mFetcher->mTextureCache->writeComplete(mCacheWriteHandle, true);
		mCacheWriteHandle = LLTextureCache::nullHandle();
	}
}

// virtual
bool LLTextureFetchWorker::deleteOK()
{
	bool delete_ok = true;
	// Allow any pending reads or writes to complete
	if (mCacheReadHandle != LLTextureCache::nullHandle())
	{
		if (mFetcher->mTextureCache->readComplete(mCacheReadHandle, true))
		{
			mCacheReadHandle = LLTextureCache::nullHandle();
		}
		else
		{
			delete_ok = false;
		}
	}
	if (mCacheWriteHandle != LLTextureCache::nullHandle())
	{
		if (mFetcher->mTextureCache->writeComplete(mCacheWriteHandle))
		{
			mCacheWriteHandle = LLTextureCache::nullHandle();
		}
		else
		{
			delete_ok = false;
		}
	}

	if ((haveWork() &&
		 // not ok to delete from these states
		 ((mState >= WRITE_TO_CACHE && mState <= WAIT_ON_WRITE))))
	{
		delete_ok = false;
	}
	
	return delete_ok;
}

void LLTextureFetchWorker::removeFromCache()
{
	if (!mInLocalCache)
	{
		mFetcher->mTextureCache->removeFromCache(mID);
	}
}


//////////////////////////////////////////////////////////////////////////////

bool LLTextureFetchWorker::processSimulatorPackets()
{
	if (mFormattedImage.isNull() || mRequestedSize < 0)
	{
		// not sure how we got here, but not a valid state, abort!
		llassert_always(mDecodeHandle == 0);
		mFormattedImage = NULL;
		return true;
	}
	
	if (mLastPacket >= mFirstPacket)
	{
		S32 buffer_size = mFormattedImage->getDataSize();
		for (S32 i = mFirstPacket; i<=mLastPacket; i++)
		{
			llassert_always(mPackets[i]);
			buffer_size += mPackets[i]->mSize;
		}
		bool have_all_data = mLastPacket >= mTotalPackets-1;
		if (mRequestedSize <= 0)
		{
			// We received a packed but haven't requested anything yet (edge case)
			// Return true (we're "done") since we didn't request anything
			return true;
		}
		if (buffer_size >= mRequestedSize || have_all_data)
		{
			/// We have enough (or all) data
			if (have_all_data)
			{
				mHaveAllData = TRUE;
			}
			S32 cur_size = mFormattedImage->getDataSize();
			if (buffer_size > cur_size)
			{
				/// We have new data
				U8* buffer = (U8*)ALLOCATE_MEM(LLImageBase::getPrivatePool(), buffer_size);
				S32 offset = 0;
				if (cur_size > 0 && mFirstPacket > 0)
				{
					memcpy(buffer, mFormattedImage->getData(), cur_size);
					offset = cur_size;
				}
				for (S32 i=mFirstPacket; i<=mLastPacket; i++)
				{
					memcpy(buffer + offset, mPackets[i]->mData, mPackets[i]->mSize);
					offset += mPackets[i]->mSize;
				}
				// NOTE: setData releases current data
				mFormattedImage->setData(buffer, buffer_size);
			}
			mLoadedDiscard = mRequestedDiscard;
			return true;
		}
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////////

S32 LLTextureFetchWorker::callbackHttpGet(const LLChannelDescriptors& channels,
										   const LLHTTPClient::ResponderBase::buffer_ptr_t& buffer,
										   bool partial, bool success)
{
	S32 data_size = 0 ;

	LLMutexLock lock(&mWorkMutex);

	if (mState != WAIT_HTTP_REQ)
	{
		llwarns << "callbackHttpGet for unrequested fetch worker: " << mID
				<< " req=" << mSentRequest << " state= " << mState << llendl;
		return data_size;
	}
	if (mLoaded)
	{
		llwarns << "Duplicate callback for " << mID.asString() << llendl;
		return data_size; // ignore duplicate callback
	}
	if (success)
	{
		// get length of stream:
		data_size = buffer->countAfter(channels.in(), NULL);

		LL_DEBUGS("Texture") << "HTTP RECEIVED: " << mID.asString() << " Bytes: " << data_size << LL_ENDL;
		if (data_size > 0)
		{
			LLViewerStatsRecorder::instance().textureFetch(data_size);
			// *TODO: set the formatted image data here directly to avoid the copy
			llassert(mHttpBuffer.empty());
			mHttpBuffer.resize(data_size);
			buffer->readAfter(channels.in(), NULL, &mHttpBuffer[0], data_size);
			if (data_size < mRequestedSize && mRequestedDiscard == 0)
			{
				mHaveAllData = TRUE;
			}
			else if (data_size > mRequestedSize)
			{
				// *TODO: This shouldn't be happening any more
				llwarns << "data_size = " << data_size << " > requested: " << mRequestedSize << llendl;
				mHaveAllData = TRUE;
				mRequestedOffset = 0;
				llassert_always(mDecodeHandle == 0);
				mFormattedImage = NULL; // discard any previous data we had
			}
		}
		else
		{
			// We requested data but received none (and no error),
			// so presumably we have all of it
			mHaveAllData = TRUE;
		}
		mRequestedSize = data_size;
	}
	else
	{
		mRequestedSize = -1; // error
	}
	mLoaded = TRUE;
	setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);

	LLViewerStatsRecorder::instance().log(0.2f);
	return data_size ;
}

//////////////////////////////////////////////////////////////////////////////

void LLTextureFetchWorker::callbackCacheRead(bool success, LLImageFormatted* image,
											 S32 imagesize, BOOL islocal)
{
	LLMutexLock lock(&mWorkMutex);
	if (mState != LOAD_FROM_TEXTURE_CACHE)
	{
// 		llwarns << "Read callback for " << mID << " with state = " << mState << llendl;
		return;
	}
	if (success)
	{
		llassert_always(imagesize >= 0);
		mFileSize = imagesize;
		mFormattedImage = image;
		mImageCodec = image->getCodec();
		mInLocalCache = islocal;
		if (mFileSize != 0 && mFormattedImage->getDataSize() >= mFileSize)
		{
			mHaveAllData = TRUE;
		}
	}
	mLoaded = TRUE;
	setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
}

void LLTextureFetchWorker::callbackCacheWrite(bool success)
{
	LLMutexLock lock(&mWorkMutex);
	if (mState != WAIT_ON_WRITE)
	{
// 		llwarns << "Write callback for " << mID << " with state = " << mState << llendl;
		return;
	}
	mWritten = TRUE;
	setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
}

//////////////////////////////////////////////////////////////////////////////

void LLTextureFetchWorker::callbackDecoded(bool success, LLImageRaw* raw, LLImageRaw* aux)
{
	LLMutexLock lock(&mWorkMutex);
	if (mDecodeHandle == 0)
	{
		return; // aborted, ignore
	}
	if (mState != DECODE_IMAGE_UPDATE)
	{
// 		llwarns << "Decode callback for " << mID << " with state = " << mState << llendl;
		mDecodeHandle = 0;
		return;
	}
	llassert_always(mFormattedImage.notNull());
	
	mDecodeHandle = 0;
	if (success)
	{
		llassert_always(raw);
		mRawImage = raw;
		mAuxImage = aux;
		mDecodedDiscard = mFormattedImage->getDiscardLevel();
 		LL_DEBUGS("Texture") << mID << ": Decode Finished. Discard: " << mDecodedDiscard
							 << " Raw Image: " << llformat("%dx%d",mRawImage->getWidth(),mRawImage->getHeight()) << LL_ENDL;
	}
	else
	{
		if (mFormattedImage.notNull())
		{
			LL_WARNS("Texture") << "DECODE FAILED: id = " << mID << ", Discard = " << (S32)mFormattedImage->getDiscardLevel() << LL_ENDL;
		}
		else
		{
			LL_WARNS("Texture") << "DECODE FAILED: id = " << mID << ", mFormattedImage is Null!" << LL_ENDL;
		}
		removeFromCache();
		mDecodedDiscard = -1; // Redundant, here for clarity and paranoia
	}
	mDecoded = TRUE;
// 	llinfos << mID << " : DECODE COMPLETE " << llendl;
	setPriority(LLWorkerThread::PRIORITY_HIGH | mWorkPriority);
	mCacheReadTime = mCacheReadTimer.getElapsedTimeF32();
}

//////////////////////////////////////////////////////////////////////////////

bool LLTextureFetchWorker::writeToCacheComplete()
{
	// Complete write to cache
	if (mCacheWriteHandle != LLTextureCache::nullHandle())
	{
		if (!mWritten)
		{
			return false;
		}
		if (mFetcher->mTextureCache->writeComplete(mCacheWriteHandle))
		{
			mCacheWriteHandle = LLTextureCache::nullHandle();
		}
		else
		{
			return false;
		}
	}
	return true;
}


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
// public

LLTextureFetch::LLTextureFetch(LLTextureCache* cache, LLImageDecodeThread* imagedecodethread, bool threaded, bool qa_mode)
	: LLWorkerThread("TextureFetch", threaded, true),
	  mDebugCount(0),
	  mDebugPause(FALSE),
	  mPacketCount(0),
	  mBadPacketCount(0),
	  mTextureCache(cache),
	  mImageDecodeThread(imagedecodethread),
	  mTextureBandwidth(0),
	  mHTTPTextureBits(0),
	  mTotalHTTPRequests(0)
{
	mTextureInfo.setUpLogging(gSavedSettings.getBOOL("LogTextureDownloadsToViewerLog"), gSavedSettings.getBOOL("LogTextureDownloadsToSimulator"), gSavedSettings.getU32("TextureLoggingThreshold"));
}

LLTextureFetch::~LLTextureFetch()
{
	clearDeleteList() ;

	
	// ~LLQueuedThread() called here
}

bool LLTextureFetch::createRequest(const std::string& url, const LLUUID& id, const LLHost& host, F32 priority,
								   S32 w, S32 h, S32 c, S32 desired_discard, bool needs_aux, bool can_use_http)
{
	if (mDebugPause)
	{
		return false;
	}
	
	LLTextureFetchWorker* worker = getWorker(id);
	if (worker)
	{
		if (worker->mHost != host)
		{
			llwarns << "LLTextureFetch::createRequest " << id << " called with multiple hosts: "
					<< host << " != " << worker->mHost << llendl;
			removeRequest(worker, true);
			worker = NULL;
			return false;
		}
	}

	S32 desired_size;
	std::string exten = gDirUtilp->getExtension(url);
	if (!url.empty() && (!exten.empty() && LLImageBase::getCodecFromExtension(exten) != IMG_CODEC_J2C))
	{
		// Only do partial requests for J2C at the moment
		//llinfos << "Merov : LLTextureFetch::createRequest(), blocking fetch on " << url << llendl; 
		desired_size = MAX_IMAGE_DATA_SIZE;
		desired_discard = 0;
	}
	else if (desired_discard == 0)
	{
		// if we want the entire image, and we know its size, then get it all
		// (calcDataSizeJ2C() below makes assumptions about how the image
		// was compressed - this code ensures that when we request the entire image,
		// we really do get it.)
		desired_size = MAX_IMAGE_DATA_SIZE;
	}
	else if (w*h*c > 0)
	{
		// If the requester knows the dimensions of the image,
		// this will calculate how much data we need without having to parse the header

		desired_size = LLImageJ2C::calcDataSizeJ2C(w, h, c, desired_discard);
	}
	else
	{
		desired_size = TEXTURE_CACHE_ENTRY_SIZE;
		desired_discard = MAX_DISCARD_LEVEL;
	}

	
	if (worker)
	{
		if (worker->wasAborted())
		{
			return false; // need to wait for previous aborted request to complete
		}
		worker->lockWorkMutex();
		worker->mActiveCount++;
		worker->mNeedsAux = needs_aux;
		worker->setImagePriority(priority);
		worker->setDesiredDiscard(desired_discard, desired_size);
		worker->setCanUseHTTP(can_use_http) ;
		if (!worker->haveWork())
		{
			worker->mState = LLTextureFetchWorker::INIT;
			worker->unlockWorkMutex();
			worker->addWork(0, LLWorkerThread::PRIORITY_HIGH | worker->mWorkPriority);
		}
		else
		{
			worker->unlockWorkMutex();
		}
	}
	else
	{
		worker = new LLTextureFetchWorker(this, url, id, host, priority, desired_discard, desired_size);
		lockQueue() ;
		mRequestMap[id] = worker;
		unlockQueue() ;

		worker->lockWorkMutex();
		worker->mActiveCount++;
		worker->mNeedsAux = needs_aux;
		worker->setCanUseHTTP(can_use_http) ;
		worker->unlockWorkMutex();
	}

 	//llinfos << "REQUESTED: " << id << " Discard: " << desired_discard << llendl;
	return true;
}

// protected
void LLTextureFetch::addToNetworkQueue(LLTextureFetchWorker* worker)
{
	lockQueue();
	bool in_request_map = (mRequestMap.find(worker->mID) != mRequestMap.end());
	unlockQueue();
	LLMutexLock lock(&mNetworkQueueMutex);
	if (in_request_map)
	{
		// only add to the queue if in the request map
		// i.e. a delete has not been requested
		mNetworkQueue.insert(worker->mID);
	}
	for (cancel_queue_t::iterator iter1 = mCancelQueue.begin();
		 iter1 != mCancelQueue.end(); ++iter1)
	{
		iter1->second.erase(worker->mID);
	}
}

void LLTextureFetch::removeFromNetworkQueue(LLTextureFetchWorker* worker, bool cancel)
{
	LLMutexLock lock(&mNetworkQueueMutex);
	size_t erased = mNetworkQueue.erase(worker->mID);
	if (cancel && erased > 0)
	{
		mCancelQueue[worker->mHost].insert(worker->mID);
	}
}

// protected
void LLTextureFetch::addToHTTPQueue(const LLUUID& id)
{
	LLMutexLock lock(&mNetworkQueueMutex);
	mHTTPTextureQueue.insert(id);
	mTotalHTTPRequests++;
}

void LLTextureFetch::removeFromHTTPQueue(const LLUUID& id, S32 received_size)
{
	LLMutexLock lock(&mNetworkQueueMutex);
	mHTTPTextureQueue.erase(id);
	mHTTPTextureBits += received_size * 8; // Approximate - does not include header bits	
}

void LLTextureFetch::deleteRequest(const LLUUID& id, bool cancel)
{
	lockQueue() ;
	LLTextureFetchWorker* worker = getWorkerAfterLock(id);

	removeRequest(worker, cancel, false);
}

void LLTextureFetch::removeRequest(LLTextureFetchWorker* worker, bool cancel, bool bNeedsLock)
{
	if(!worker)
	{
		if(!bNeedsLock)
			unlockQueue() ;
		return;
	}
	if(bNeedsLock)
		lockQueue() ;

	size_t erased_1 = mRequestMap.erase(worker->mID);
	unlockQueue() ;

	llassert_always(erased_1 > 0) ;
	removeFromNetworkQueue(worker, cancel);
	llassert_always(!(worker->getFlags(LLWorkerClass::WCF_DELETE_REQUESTED))) ;

	worker->scheduleDelete();	
}

void LLTextureFetch::deleteAllRequests()
{
	while(1)
	{
		lockQueue();
		if(mRequestMap.empty())
		{
			unlockQueue() ;
			break;
		}

		LLTextureFetchWorker* worker = mRequestMap.begin()->second;

		removeRequest(worker, true, false);
	}
}

S32 LLTextureFetch::getNumRequests() 
{ 
	lockQueue() ;
	S32 size = (S32)mRequestMap.size(); 
	unlockQueue() ;

	return size ;
}

S32 LLTextureFetch::getNumHTTPRequests() 
{ 
	mNetworkQueueMutex.lock() ;
	S32 size = (S32)mHTTPTextureQueue.size(); 
	mNetworkQueueMutex.unlock() ;

	return size ;
}

U32 LLTextureFetch::getTotalNumHTTPRequests()
{
	mNetworkQueueMutex.lock() ;
	U32 size = mTotalHTTPRequests ;
	mNetworkQueueMutex.unlock() ;

	return size ;
}

// call lockQueue() first!
LLTextureFetchWorker* LLTextureFetch::getWorkerAfterLock(const LLUUID& id)
{
	LLTextureFetchWorker* res = NULL;
	map_t::iterator iter = mRequestMap.find(id);
	if (iter != mRequestMap.end())
	{
		res = iter->second;
	}
	return res;
}

LLTextureFetchWorker* LLTextureFetch::getWorker(const LLUUID& id)
{
	LLMutexLock lock(&mQueueMutex) ;

	return getWorkerAfterLock(id) ;
}


bool LLTextureFetch::getRequestFinished(const LLUUID& id, S32& discard_level,
										LLPointer<LLImageRaw>& raw, LLPointer<LLImageRaw>& aux)
{
	bool res = false;
	LLTextureFetchWorker* worker = getWorker(id);
	if (worker)
	{
		if (worker->wasAborted())
		{
			res = true;
		}
		else if (!worker->haveWork())
		{
			// Should only happen if we set mDebugPause...
			if (!mDebugPause)
			{
// 				llwarns << "Adding work for inactive worker: " << id << llendl;
				worker->addWork(0, LLWorkerThread::PRIORITY_HIGH | worker->mWorkPriority);
			}
		}
		else if (worker->checkWork())
		{
			worker->lockWorkMutex();
			discard_level = worker->mDecodedDiscard;
			raw = worker->mRawImage;
			aux = worker->mAuxImage;
			F32 cache_read_time = worker->mCacheReadTime;
			if (cache_read_time != 0.f)
			{
				sCacheReadLatency.addValue(cache_read_time * 1000.f);
			}
			res = true;
			LL_DEBUGS("Texture") << id << ": Request Finished. State: " << worker->mState << " Discard: " << discard_level << LL_ENDL;
			worker->unlockWorkMutex();
		}
		else
		{
			worker->lockWorkMutex();
			if ((worker->mDecodedDiscard >= 0) &&
				(worker->mDecodedDiscard < discard_level || discard_level < 0) &&
				(worker->mState >= LLTextureFetchWorker::WAIT_ON_WRITE))
			{
				// Not finished, but data is ready
				discard_level = worker->mDecodedDiscard;
				raw = worker->mRawImage;
				aux = worker->mAuxImage;
			}
			worker->unlockWorkMutex();
		}
	}
	else
	{
		res = true;
	}
	return res;
}

bool LLTextureFetch::updateRequestPriority(const LLUUID& id, F32 priority)
{
	bool res = false;
	LLTextureFetchWorker* worker = getWorker(id);
	if (worker)
	{
		worker->lockWorkMutex();
		worker->setImagePriority(priority);
		worker->unlockWorkMutex();
		res = true;
	}
	return res;
}

//
// May be called from any thread

//virtual
S32 LLTextureFetch::getPending()
{
	S32 res;
	lockData();
    {
        LLMutexLock lock(&mQueueMutex);
        
        res = mRequestQueue.size();
    }
	unlockData();
	return res;
}

// virtual
bool LLTextureFetch::runCondition()
{
	// Caller is holding the lock on LLThread's condition variable.
	
	// LLQueuedThread, unlike its base class LLThread, makes this a
	// private method which is unfortunate.  I want to use it directly
	// but I'm going to have to re-implement the logic here (or change
	// declarations, which I don't want to do right now).
	//
	// Changes here may need to be reflected in getPending().
	
	return ! (mRequestQueue.empty() && mIdleThread);		// From base class
}

//////////////////////////////////////////////////////////////////////////////

// MAIN THREAD (unthreaded envs), WORKER THREAD (threaded envs)
void LLTextureFetch::commonUpdate()
{
}


// MAIN THREAD
//virtual
S32 LLTextureFetch::update(F32 max_time_ms)
{
	{
		mNetworkQueueMutex.lock() ;

		gTextureList.sTextureBits += mHTTPTextureBits ;
		mHTTPTextureBits = 0 ;

		mNetworkQueueMutex.unlock() ;
	}

	S32 res = LLWorkerThread::update(max_time_ms);
	
	if (!mDebugPause)
	{
		// this is the startup state when send_complete_agent_movement() message is sent.
		// Before this, the RequestImages message sent by sendRequestListToSimulators 
		// won't work so don't bother trying
		if (LLStartUp::getStartupState() > STATE_AGENT_SEND)
		{
			sendRequestListToSimulators();
		}
	}

	if (!mThreaded)
	{
		commonUpdate();
	}

	return res;
}

//called in the MAIN thread after the TextureCacheThread shuts down.
void LLTextureFetch::shutDownTextureCacheThread() 
{
	if(mTextureCache)
	{
		llassert_always(mTextureCache->isQuitting() || mTextureCache->isStopped()) ;
		mTextureCache = NULL ;
	}
}
	
//called in the MAIN thread after the ImageDecodeThread shuts down.
void LLTextureFetch::shutDownImageDecodeThread() 
{
	if(mImageDecodeThread)
	{
		llassert_always(mImageDecodeThread->isQuitting() || mImageDecodeThread->isStopped()) ;
		mImageDecodeThread = NULL ;
	}
}

// WORKER THREAD
void LLTextureFetch::threadedUpdate()
{
	// Limit update frequency
	const F32 PROCESS_TIME = 0.05f; 
	static LLFrameTimer process_timer;
	if (process_timer.getElapsedTimeF32() < PROCESS_TIME)
	{
		return;
	}
	process_timer.reset();
	
	commonUpdate();

#if 0
	const F32 INFO_TIME = 1.0f; 
	static LLFrameTimer info_timer;
	if (info_timer.getElapsedTimeF32() >= INFO_TIME)
	{
		S32 q = mCurlGetRequest->getQueued();
		if (q > 0)
		{
			llinfos << "Queued gets: " << q << llendl;
			info_timer.reset();
		}
	}
#endif
	
}

//////////////////////////////////////////////////////////////////////////////

void LLTextureFetch::sendRequestListToSimulators()
{
	// All requests
	const F32 REQUEST_DELTA_TIME = 0.10f; // 10 fps
	
	// Sim requests
	const S32 IMAGES_PER_REQUEST = 50;
	const F32 SIM_LAZY_FLUSH_TIMEOUT = 10.0f; // temp
	const F32 MIN_REQUEST_TIME = 1.0f;
	const F32 MIN_DELTA_PRIORITY = 1000.f;

	// Periodically, gather the list of textures that need data from the network
	// And send the requests out to the simulators
	static LLFrameTimer timer;
	if (timer.getElapsedTimeF32() < REQUEST_DELTA_TIME)
	{
		return;
	}
	timer.reset();
	
	// Send requests
	typedef std::set<LLTextureFetchWorker*,LLTextureFetchWorker::Compare> request_list_t;
	typedef std::map< LLHost, request_list_t > work_request_map_t;
	work_request_map_t requests;
	{
	LLMutexLock lock2(&mNetworkQueueMutex);
	for (queue_t::iterator iter = mNetworkQueue.begin(); iter != mNetworkQueue.end(); )
	{
		queue_t::iterator curiter = iter++;
		LLTextureFetchWorker* req = getWorker(*curiter);
		if (!req)
		{
			// This happens when a request was removed from mRequestMap in a race
			// with adding it to mNetworkQueue by doWork (see SNOW-196).
			mNetworkQueue.erase(curiter);
			continue;
		}
		if ((req->mState != LLTextureFetchWorker::LOAD_FROM_NETWORK) &&
			(req->mState != LLTextureFetchWorker::LOAD_FROM_SIMULATOR))
		{
			// We already received our URL, remove from the queue
			llwarns << "Worker: " << req->mID << " in mNetworkQueue but in wrong state: " << req->mState << llendl;
			mNetworkQueue.erase(curiter);
			continue;
		}
		if (req->mID == mDebugID)
		{
			mDebugCount++; // for setting breakpoints
		}
		if (req->mSentRequest == LLTextureFetchWorker::SENT_SIM &&
			req->mTotalPackets > 0 &&
			req->mLastPacket >= req->mTotalPackets-1)
		{
			// We have all the packets... make sure this is high priority
// 			req->setPriority(LLWorkerThread::PRIORITY_HIGH | req->mWorkPriority);
			continue;
		}
		F32 elapsed = req->mRequestedTimer.getElapsedTimeF32();
		{
			F32 delta_priority = llabs(req->mRequestedPriority - req->mImagePriority);
			if ((req->mSimRequestedDiscard != req->mDesiredDiscard) ||
				(delta_priority > MIN_DELTA_PRIORITY && elapsed >= MIN_REQUEST_TIME) ||
				(elapsed >= SIM_LAZY_FLUSH_TIMEOUT))
			{
				requests[req->mHost].insert(req);
			}
		}
	}
	}

	for (work_request_map_t::iterator iter1 = requests.begin();
		 iter1 != requests.end(); ++iter1)
	{
		LLHost host = iter1->first;
		// invalid host = use agent host
		if (host == LLHost::invalid)
		{
			host = gAgent.getRegionHost();
		}

		S32 sim_request_count = 0;
		
		for (request_list_t::iterator iter2 = iter1->second.begin();
			 iter2 != iter1->second.end(); ++iter2)
		{
			LLTextureFetchWorker* req = *iter2;
			if (gMessageSystem)
			{
				if (req->mSentRequest != LLTextureFetchWorker::SENT_SIM)
				{
					// Initialize packet data based on data read from cache
					req->lockWorkMutex();
					req->setupPacketData();
					req->unlockWorkMutex();
				}
				if (0 == sim_request_count)
				{
					gMessageSystem->newMessageFast(_PREHASH_RequestImage);
					gMessageSystem->nextBlockFast(_PREHASH_AgentData);
					gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
					gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
				}
				S32 packet = req->mLastPacket + 1;
				gMessageSystem->nextBlockFast(_PREHASH_RequestImage);
				gMessageSystem->addUUIDFast(_PREHASH_Image, req->mID);
				gMessageSystem->addS8Fast(_PREHASH_DiscardLevel, (S8)req->mDesiredDiscard);
				gMessageSystem->addF32Fast(_PREHASH_DownloadPriority, req->mImagePriority);
				gMessageSystem->addU32Fast(_PREHASH_Packet, packet);
				gMessageSystem->addU8Fast(_PREHASH_Type, req->mType);
// 				llinfos << "IMAGE REQUEST: " << req->mID << " Discard: " << req->mDesiredDiscard
// 						<< " Packet: " << packet << " Priority: " << req->mImagePriority << llendl;

				static LLCachedControl<bool> log_to_viewer_log(gSavedSettings,"LogTextureDownloadsToViewerLog");
				static LLCachedControl<bool> log_to_sim(gSavedSettings,"LogTextureDownloadsToSimulator");
				if (log_to_viewer_log || log_to_sim)
				{
					mTextureInfo.setRequestStartTime(req->mID, LLTimer::getTotalTime());
					mTextureInfo.setRequestOffset(req->mID, 0);
					mTextureInfo.setRequestSize(req->mID, 0);
					mTextureInfo.setRequestType(req->mID, LLTextureInfoDetails::REQUEST_TYPE_UDP);
				}

				req->lockWorkMutex();
				req->mSentRequest = LLTextureFetchWorker::SENT_SIM;
				req->mSimRequestedDiscard = req->mDesiredDiscard;
				req->mRequestedPriority = req->mImagePriority;
				req->mRequestedTimer.reset();
				req->unlockWorkMutex();
				sim_request_count++;
				if (sim_request_count >= IMAGES_PER_REQUEST)
				{
// 					llinfos << "REQUESTING " << sim_request_count << " IMAGES FROM HOST: " << host.getIPString() << llendl;

					gMessageSystem->sendSemiReliable(host, NULL, NULL);
					sim_request_count = 0;
				}
			}
		}
		if (gMessageSystem && sim_request_count > 0 && sim_request_count < IMAGES_PER_REQUEST)
		{
// 			llinfos << "REQUESTING " << sim_request_count << " IMAGES FROM HOST: " << host.getIPString() << llendl;
			gMessageSystem->sendSemiReliable(host, NULL, NULL);
			sim_request_count = 0;
		}
	}
	
	// Send cancelations
	{
	LLMutexLock lock2(&mNetworkQueueMutex);
	if (gMessageSystem && !mCancelQueue.empty())
	{
		for (cancel_queue_t::iterator iter1 = mCancelQueue.begin();
			 iter1 != mCancelQueue.end(); ++iter1)
		{
			LLHost host = iter1->first;
			if (host == LLHost::invalid)
			{
				host = gAgent.getRegionHost();
			}
			S32 request_count = 0;
			for (queue_t::iterator iter2 = iter1->second.begin();
				 iter2 != iter1->second.end(); ++iter2)
			{
				if (0 == request_count)
				{
					gMessageSystem->newMessageFast(_PREHASH_RequestImage);
					gMessageSystem->nextBlockFast(_PREHASH_AgentData);
					gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
					gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
				}
				gMessageSystem->nextBlockFast(_PREHASH_RequestImage);
				gMessageSystem->addUUIDFast(_PREHASH_Image, *iter2);
				gMessageSystem->addS8Fast(_PREHASH_DiscardLevel, -1);
				gMessageSystem->addF32Fast(_PREHASH_DownloadPriority, 0);
				gMessageSystem->addU32Fast(_PREHASH_Packet, 0);
				gMessageSystem->addU8Fast(_PREHASH_Type, 0);
// 				llinfos << "CANCELING IMAGE REQUEST: " << (*iter2) << llendl;

				request_count++;
				if (request_count >= IMAGES_PER_REQUEST)
				{
					gMessageSystem->sendSemiReliable(host, NULL, NULL);
					request_count = 0;
				}
			}
			if (request_count > 0 && request_count < IMAGES_PER_REQUEST)
			{
				gMessageSystem->sendSemiReliable(host, NULL, NULL);
			}
		}
		mCancelQueue.clear();
	}
	}
}

//////////////////////////////////////////////////////////////////////////////

bool LLTextureFetchWorker::insertPacket(S32 index, U8* data, S32 size)
{
	mRequestedTimer.reset();
	if (index >= mTotalPackets)
	{
// 		llwarns << "Received Image Packet " << index << " > max: " << mTotalPackets << " for image: " << mID << llendl;
		return false;
	}
	if (index > 0 && index < mTotalPackets-1 && size != MAX_IMG_PACKET_SIZE)
	{
// 		llwarns << "Received bad sized packet: " << index << ", " << size << " != " << MAX_IMG_PACKET_SIZE << " for image: " << mID << llendl;
		return false;
	}
	
	if (index >= (S32)mPackets.size())
	{
		mPackets.resize(index+1, (PacketData*)NULL); // initializes v to NULL pointers
	}
	else if (mPackets[index] != NULL)
	{
// 		llwarns << "Received duplicate packet: " << index << " for image: " << mID << llendl;
		return false;
	}

	mPackets[index] = new PacketData(data, size);
	while (mLastPacket+1 < (S32)mPackets.size() && mPackets[mLastPacket+1] != NULL)
	{
		++mLastPacket;
	}
	return true;
}

bool LLTextureFetch::receiveImageHeader(const LLHost& host, const LLUUID& id, U8 codec, U16 packets, U32 totalbytes,
										U16 data_size, U8* data)
{
	LLTextureFetchWorker* worker = getWorker(id);
	bool res = true;

	++mPacketCount;
	
	if (!worker)
	{
// 		llwarns << "Received header for non active worker: " << id << llendl;
		res = false;
	}
	else if (worker->mState != LLTextureFetchWorker::LOAD_FROM_NETWORK ||
			 worker->mSentRequest != LLTextureFetchWorker::SENT_SIM)
	{
// 		llwarns << "receiveImageHeader for worker: " << id
// 				<< " in state: " << LLTextureFetchWorker::sStateDescs[worker->mState]
// 				<< " sent: " << worker->mSentRequest << llendl;
		res = false;
	}
	else if (worker->mLastPacket != -1)
	{
		// check to see if we've gotten this packet before
// 		llwarns << "Received duplicate header for: " << id << llendl;
		res = false;
	}
	else if (!data_size)
	{
// 		llwarns << "Img: " << id << ":" << " Empty Image Header" << llendl;
		res = false;
	}
	if (!res)
	{
		++mBadPacketCount;
		mNetworkQueueMutex.lock() ;
		mCancelQueue[host].insert(id);
		mNetworkQueueMutex.unlock() ;
		return false;
	}

	LLViewerStatsRecorder::instance().textureFetch(data_size);
	LLViewerStatsRecorder::instance().log(0.1f);

	worker->lockWorkMutex();


	//	Copy header data into image object
	worker->mImageCodec = codec;
	worker->mTotalPackets = packets;
		worker->mFileSize = (S32)totalbytes;	
		llassert_always(totalbytes > 0);
		llassert_always(data_size == FIRST_PACKET_SIZE || data_size == worker->mFileSize);
		res = worker->insertPacket(0, data, data_size);
		worker->setPriority(LLWorkerThread::PRIORITY_HIGH | worker->mWorkPriority);
		worker->mState = LLTextureFetchWorker::LOAD_FROM_SIMULATOR;
		worker->unlockWorkMutex();
		return res;
}

bool LLTextureFetch::receiveImagePacket(const LLHost& host, const LLUUID& id, U16 packet_num, U16 data_size, U8* data)
{
	LLTextureFetchWorker* worker = getWorker(id);
	bool res = true;

	++mPacketCount;
	
	if (!worker)
	{
// 		llwarns << "Received packet " << packet_num << " for non active worker: " << id << llendl;
		res = false;
	}
	else if (worker->mLastPacket == -1)
	{
// 		llwarns << "Received packet " << packet_num << " before header for: " << id << llendl;
		res = false;
	}
	else if (!data_size)
	{
// 		llwarns << "Img: " << id << ":" << " Empty Image Header" << llendl;
		res = false;
	}
	if (!res)
	{
		++mBadPacketCount;
		mNetworkQueueMutex.lock() ;
		mCancelQueue[host].insert(id);
		mNetworkQueueMutex.unlock() ;
		return false;
	}
	
	LLViewerStatsRecorder::instance().textureFetch(data_size);
	LLViewerStatsRecorder::instance().log(0.1f);

	worker->lockWorkMutex();

	
	res = worker->insertPacket(packet_num, data, data_size);
	
	if ((worker->mState == LLTextureFetchWorker::LOAD_FROM_SIMULATOR) ||
		(worker->mState == LLTextureFetchWorker::LOAD_FROM_NETWORK))
	{
		worker->setPriority(LLWorkerThread::PRIORITY_HIGH | worker->mWorkPriority);
		worker->mState = LLTextureFetchWorker::LOAD_FROM_SIMULATOR;
	}
	else
	{
// 		llwarns << "receiveImagePacket " << packet_num << "/" << worker->mLastPacket << " for worker: " << id
// 				<< " in state: " << LLTextureFetchWorker::sStateDescs[worker->mState] << llendl;
		removeFromNetworkQueue(worker, true); // failsafe
	}

	if(packet_num >= (worker->mTotalPackets - 1))
	{
		static LLCachedControl<bool> log_to_viewer_log(gSavedSettings,"LogTextureDownloadsToViewerLog");
		static LLCachedControl<bool> log_to_sim(gSavedSettings,"LogTextureDownloadsToSimulator");

		if (log_to_viewer_log || log_to_sim)
		{
			U64 timeNow = LLTimer::getTotalTime();
			mTextureInfo.setRequestSize(id, worker->mFileSize);
			mTextureInfo.setRequestCompleteTimeAndLog(id, timeNow);
		}
	}
	worker->unlockWorkMutex();

	return res;
}

//////////////////////////////////////////////////////////////////////////////
BOOL LLTextureFetch::isFromLocalCache(const LLUUID& id)
{
	BOOL from_cache = FALSE;

	LLTextureFetchWorker* worker = getWorker(id);
	if (worker)
	{
		worker->lockWorkMutex();
		from_cache = worker->mInLocalCache;
		worker->unlockWorkMutex();
	}

	return from_cache;
}

S32 LLTextureFetch::getFetchState(const LLUUID& id, F32& data_progress_p, F32& requested_priority_p,
								  U32& fetch_priority_p, F32& fetch_dtime_p, F32& request_dtime_p, bool& can_use_http)
{
	S32 state = LLTextureFetchWorker::INVALID;
	F32 data_progress = 0.0f;
	F32 requested_priority = 0.0f;
	F32 fetch_dtime = 999999.f;
	F32 request_dtime = 999999.f;
	U32 fetch_priority = 0;
	
	LLTextureFetchWorker* worker = getWorker(id);
	if (worker && worker->haveWork())
	{
		worker->lockWorkMutex();
		state = worker->mState;
		fetch_dtime = worker->mFetchTimer.getElapsedTimeF32();
		request_dtime = worker->mRequestedTimer.getElapsedTimeF32();
		if (worker->mFileSize > 0)
		{
			if (state == LLTextureFetchWorker::LOAD_FROM_SIMULATOR)
			{
				S32 data_size = FIRST_PACKET_SIZE + (worker->mLastPacket-1) * MAX_IMG_PACKET_SIZE;
				data_size = llmax(data_size, 0);
				data_progress = (F32)data_size / (F32)worker->mFileSize;
			}
			else if (worker->mFormattedImage.notNull())
			{
				data_progress = (F32)worker->mFormattedImage->getDataSize() / (F32)worker->mFileSize;
			}
		}
		if (state >= LLTextureFetchWorker::LOAD_FROM_NETWORK && state <= LLTextureFetchWorker::WAIT_HTTP_REQ)
		{
			requested_priority = worker->mRequestedPriority;
		}
		else
		{
			requested_priority = worker->mImagePriority;
		}
		fetch_priority = worker->getPriority();
		can_use_http = worker->getCanUseHTTP() ;
		worker->unlockWorkMutex();
	}
	data_progress_p = data_progress;
	requested_priority_p = requested_priority;
	fetch_priority_p = fetch_priority;
	fetch_dtime_p = fetch_dtime;
	request_dtime_p = request_dtime;
	return state;
}

void LLTextureFetch::dump()
{
	llinfos << "LLTextureFetch REQUESTS:" << llendl;
	for (request_queue_t::iterator iter = mRequestQueue.begin();
		 iter != mRequestQueue.end(); ++iter)
	{
		LLQueuedThread::QueuedRequest* qreq = *iter;
		LLWorkerThread::WorkRequest* wreq = (LLWorkerThread::WorkRequest*)qreq;
		LLTextureFetchWorker* worker = (LLTextureFetchWorker*)wreq->getWorkerClass();
		llinfos << " ID: " << worker->mID
				<< " PRI: " << llformat("0x%08x",wreq->getPriority())
				<< " STATE: " << worker->sStateDescs[worker->mState]
				<< llendl;
	}

	llinfos << "LLTextureFetch ACTIVE_HTTP:" << llendl;
	for (queue_t::const_iterator iter(mHTTPTextureQueue.begin());
		 mHTTPTextureQueue.end() != iter;
		 ++iter)
	{
		llinfos << " ID: " << (*iter) << llendl;
	}
}
