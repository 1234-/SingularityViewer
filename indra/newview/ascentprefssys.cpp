/** 
 * @file hbprefsinert.cpp
 * @author Henri Beauchamp
 * @brief Ascent Viewer preferences panel
 *
 * $LicenseInfo:firstyear=2008&license=viewergpl$
 * 
 * Copyright (c) 2008, Henri Beauchamp.
 * 
 * Second Life Viewer Source Code
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

//File include
#include "ascentprefssys.h"
#include "llcolorswatch.h"
#include "llvoavatar.h"
#include "llagent.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "lluictrlfactory.h"
#include "llcombobox.h"
#include "llwind.h"
#include "llviewernetwork.h"
#include "pipeline.h"

//System page ------------------------------------------------------------------------------ -HgB
class LLPrefsAscentSysImpl : public LLPanel
{
public:
	LLPrefsAscentSysImpl();
	/*virtual*/ ~LLPrefsAscentSysImpl() { };

	virtual void refresh();

	void apply();
	void cancel();

private:
	static void onCommitCheckBox(LLUICtrl* ctrl, void* user_data);
	void refreshValues();
	//General -----------------------------------------------------------------------------
	BOOL mDoubleClickTeleport;
	BOOL mPreviewAnimInWorld;
	BOOL mSaveScriptsAsMono;
	BOOL mAlwaysRezInGroup;
	//Chat/IM -----------------------------------------------------------------------------
	BOOL mHideNotificationsInChat;
	BOOL mPlayTypingSound;
	BOOL mHideTypingNotification;
	BOOL mEnableMUPose;
	BOOL mEnableOOCAutoClose;
	U32 mLinksForChattingObjects;
	U32 mTimeFormat;
	U32 mDateFormat;
	BOOL mSecondsInChatAndIMs;
	//Performance -------------------------------------------------------------------------
	BOOL mFetchInventoryOnLogin;
	BOOL mEnableLLWind;
	BOOL mEnableClouds;
	BOOL mSpeedRez;
	U32 mSpeedRezInterval;
	//Command Line ------------------------------------------------------------------------
	//Privacy -----------------------------------------------------------------------------
	BOOL mBroadcastViewerEffects;
	BOOL mDisablePointAtAndBeam;
	BOOL mPrivateLookAt;
	BOOL mRevokePermsOnStandUp;
};


LLPrefsAscentSysImpl::LLPrefsAscentSysImpl()
 : LLPanel(std::string("Ascent"))
{
	LLUICtrlFactory::getInstance()->buildPanel(this, "panel_preferences_ascent_system.xml");
	childSetCommitCallback("speed_rez_check", onCommitCheckBox, this);
	
	refresh();
}

//static
void LLPrefsAscentSysImpl::onCommitCheckBox(LLUICtrl* ctrl, void* user_data)
{
	LLPrefsAscentSysImpl* self = (LLPrefsAscentSysImpl*)user_data;

	self->childEnable("fetch_inventory_on_login_check");	
	
	llinfos << "Change to " << ctrl->getControlName()  << llendl;
	
	if (ctrl->getControlName() == "SpeedRez")
	{
		if (self->childGetValue("speed_rez_check").asBoolean())
		{
			self->childEnable("speed_rez_interval");
			self->childEnable("speed_rez_seconds");
		}
		else
		{
			self->childDisable("speed_rez_interval");
			self->childDisable("speed_rez_seconds");
		}
	}
}

void LLPrefsAscentSysImpl::refreshValues()
{
	//General -----------------------------------------------------------------------------
	mDoubleClickTeleport		= gSavedSettings.getBOOL("DoubleClickTeleport");
	mPreviewAnimInWorld			= gSavedSettings.getBOOL("PreviewAnimInWorld");
	mSaveScriptsAsMono			= gSavedSettings.getBOOL("SaveScriptsAsMono");
	mAlwaysRezInGroup			= gSavedSettings.getBOOL("AscentAlwaysRezInGroup");
	//Chat/IMs ----------------------------------------------------------------------------
	mHideNotificationsInChat	= gSavedSettings.getBOOL("HideNotificationsInChat");
	mHideTypingNotification		= gSavedSettings.getBOOL("AscentHideTypingNotification");
	mPlayTypingSound			= gSavedSettings.getBOOL("PlayTypingSound");
	mEnableMUPose				= gSavedSettings.getBOOL("AscentAllowMUpose");
	mEnableOOCAutoClose			= gSavedSettings.getBOOL("AscentAutoCloseOOC");
	mLinksForChattingObjects	= gSavedSettings.getU32("LinksForChattingObjects");
	//Time format
	//Date Format
	mSecondsInChatAndIMs		= gSavedSettings.getBOOL("SecondsInChatAndIMs");
	
	//Performance -------------------------------------------------------------------------
	mFetchInventoryOnLogin		= gSavedSettings.getBOOL("FetchInventoryOnLogin");
	mEnableLLWind				= gSavedSettings.getBOOL("WindEnabled");
	mEnableClouds				= gSavedSettings.getBOOL("CloudsEnabled");
	mSpeedRez					= gSavedSettings.getBOOL("SpeedRez");
	mSpeedRezInterval			= gSavedSettings.getU32("SpeedRezInterval");

	//Command Line ------------------------------------------------------------------------


	//Privacy -----------------------------------------------------------------------------
	mBroadcastViewerEffects		= gSavedSettings.getBOOL("BroadcastViewerEffects");
	mDisablePointAtAndBeam		= gSavedSettings.getBOOL("DisablePointAtAndBeam");
	mPrivateLookAt				= gSavedSettings.getBOOL("PrivateLookAt");
	mRevokePermsOnStandUp		= gSavedSettings.getBOOL("RevokePermsOnStandUp");
}

void LLPrefsAscentSysImpl::refresh()
{
	refreshValues();
	
	//General -----------------------------------------------------------------------------
	childSetValue("double_click_teleport_check",	mDoubleClickTeleport);
	childSetValue("preview_anim_in_world_check",	mPreviewAnimInWorld);
	childSetValue("save_scripts_as_mono_check",		mSaveScriptsAsMono);
	childSetValue("always_rez_in_group_check",		mAlwaysRezInGroup);
	//Chat --------------------------------------------------------------------------------
	childSetValue("hide_notifications_in_chat_check", mHideNotificationsInChat);
	childSetValue("play_typing_sound_check",		mPlayTypingSound);
	childSetValue("hide_typing_check",				mHideTypingNotification);
	childSetValue("seconds_in_chat_and_ims_check",	mSecondsInChatAndIMs);
	childSetValue("allow_mu_pose_check",			mEnableMUPose);
	childSetValue("close_ooc_check",				mEnableOOCAutoClose);
	//Show Links
	//Time Format
	//Date Format
	childSetValue("seconds_in_chat_and_ims_check",	mEnableOOCAutoClose);
	//Save Performance --------------------------------------------------------------------
	childSetValue("fetch_inventory_on_login_check", mFetchInventoryOnLogin);
	childSetValue("enable_wind", mEnableLLWind);
	childSetValue("enable_clouds", mEnableClouds);
	childSetValue("speed_rez_check", mSpeedRez);
	if (mSpeedRez)
	{
		childEnable("speed_rez_interval");
		childEnable("speed_rez_seconds");
	}
	else
	{
		childDisable("speed_rez_interval");
		childDisable("speed_rez_seconds");
	}
	//Command Line ------------------------------------------------------------------------

	//Privacy -----------------------------------------------------------------------------
	childSetValue("broadcast_viewer_effects", mBroadcastViewerEffects);
	childSetValue("disable_point_at_and_beams_check", mDisablePointAtAndBeam);
	childSetValue("private_look_at_check", mPrivateLookAt);
	childSetValue("revoke_perms_on_stand_up_check", mRevokePermsOnStandUp);

	std::string format = gSavedSettings.getString("ShortTimeFormat");
	if (format.find("%p") == -1)
	{
		mTimeFormat = 0;
	}
	else
	{
		mTimeFormat = 1;
	}

	format = gSavedSettings.getString("ShortDateFormat");
	if (format.find("%m/%d/%") != -1)
	{
		mDateFormat = 2;
	}
	else if (format.find("%d/%m/%") != -1)
	{
		mDateFormat = 1;
	}
	else
	{
		mDateFormat = 0;
	}

	// time format combobox
	LLComboBox* combo = getChild<LLComboBox>("time_format_combobox");
	if (combo)
	{
		combo->setCurrentByIndex(mTimeFormat);
	}

	// date format combobox
	combo = getChild<LLComboBox>("date_format_combobox");
	if (combo)
	{
		combo->setCurrentByIndex(mDateFormat);
	}
}

void LLPrefsAscentSysImpl::cancel()
{
	//General -----------------------------------------------------------------------------
	childSetValue("double_click_teleport_check",	mDoubleClickTeleport);
	childSetValue("preview_anim_in_world_check",	mPreviewAnimInWorld);
	childSetValue("save_scripts_as_mono_check",		mSaveScriptsAsMono);
	childSetValue("always_rez_in_group_check",		mAlwaysRezInGroup);
	//Chat --------------------------------------------------------------------------------
	childSetValue("hide_notifications_in_chat_check", mHideNotificationsInChat);
	childSetValue("play_typing_sound_check",		mPlayTypingSound);
	childSetValue("hide_typing_check",				mHideTypingNotification);
	childSetValue("seconds_in_chat_and_ims_check",	mSecondsInChatAndIMs);
	childSetValue("allow_mu_pose_check",			mEnableMUPose);
	childSetValue("close_ooc_check",				mEnableOOCAutoClose);
	//Show Links
	//Time Format
	//Date Format
	childSetValue("seconds_in_chat_and_ims_check",	mEnableOOCAutoClose);
	//Save Performance --------------------------------------------------------------------
	childSetValue("fetch_inventory_on_login_check", mFetchInventoryOnLogin);
	childSetValue("enable_wind", mEnableLLWind);
	childSetValue("enable_clouds", mEnableClouds);
	childSetValue("speed_rez_check", mSpeedRez);
	if (mSpeedRez)
	{
		childEnable("speed_rez_interval");
		childEnable("speed_rez_seconds");
	}
	else
	{
		childDisable("speed_rez_interval");
		childDisable("speed_rez_seconds");
	}
	//Command Line ------------------------------------------------------------------------

	//Privacy -----------------------------------------------------------------------------
	childSetValue("broadcast_viewer_effects", mBroadcastViewerEffects);
	childSetValue("disable_point_at_and_beams_check", mDisablePointAtAndBeam);
	childSetValue("private_look_at_check", mPrivateLookAt);
	childSetValue("revoke_perms_on_stand_up_check", mRevokePermsOnStandUp);
	
	if(mEnableClouds != gSavedSettings.getBOOL("CloudsEnabled"))
	{
		gSavedSettings.setBOOL("CloudsEnabled", mEnableClouds);
		LLPipeline::toggleRenderTypeControl((void*)LLPipeline::RENDER_TYPE_CLOUDS);
	}
}

void LLPrefsAscentSysImpl::apply()
{
	std::string short_date, long_date, short_time, long_time, timestamp;
	
	//General ------------------------------------------------------------------------------
	gSavedSettings.setBOOL("DoubleClickTeleport",		childGetValue("double_click_teleport_check"));
	gSavedSettings.setBOOL("PreviewAnimInWorld",		childGetValue("preview_anim_in_world_check"));
	gSavedSettings.setBOOL("SaveScriptsAsMono",			childGetValue("save_scripts_as_mono_check"));
	gSavedSettings.setBOOL("AscentAlwaysRezInGroup",	childGetValue("always_rez_in_group_check"));

	//Chat/IM ------------------------------------------------------------------------------
	//Missing the echo/log option.
	gSavedSettings.setBOOL("PlayTypingSound",			childGetValue("play_typing_sound_check"));
	gSavedSettings.setBOOL("AscentHideTypingNotification",	childGetValue("hide_typing_check"));
	gSavedSettings.setBOOL("AscentAllowMUpose",			childGetValue("allow_mu_pose_check"));
	gSavedSettings.setBOOL("AscentAutoCloseOOC",		childGetValue("close_ooc_check"));
	gSavedSettings.setU32("LinksForChattingObjects",	childGetValue("objects_link").asInteger());
	
	LLComboBox* combo = getChild<LLComboBox>("time_format_combobox");
	if (combo) {
		mTimeFormat = combo->getCurrentIndex();
	}

	combo = getChild<LLComboBox>("date_format_combobox");
	if (combo)
	{
		mDateFormat = combo->getCurrentIndex();
	}

	if (mTimeFormat == 0)
	{
		short_time = "%H:%M";
		long_time  = "%H:%M:%S";
		timestamp  = " %H:%M:%S";
	}
	else
	{
		short_time = "%I:%M %p";
		long_time  = "%I:%M:%S %p";
		timestamp  = " %I:%M %p";
	}

	if (mDateFormat == 0)
	{
		short_date = "%Y-%m-%d";
		long_date  = "%A %d %B %Y";
		timestamp  = "%a %d %b %Y" + timestamp;
	}
	else if (mDateFormat == 1)
	{
		short_date = "%d/%m/%Y";
		long_date  = "%A %d %B %Y";
		timestamp  = "%a %d %b %Y" + timestamp;
	}
	else
	{
		short_date = "%m/%d/%Y";
		long_date  = "%A, %B %d %Y";
		timestamp  = "%a %b %d %Y" + timestamp;
	}

	gSavedSettings.setString("ShortDateFormat",			short_date);
	gSavedSettings.setString("LongDateFormat",			long_date);
	gSavedSettings.setString("ShortTimeFormat",			short_time);
	gSavedSettings.setString("LongTimeFormat",			long_time);
	gSavedSettings.setString("TimestampFormat",			timestamp);

	gSavedSettings.setBOOL("SecondsInChatAndIMs",		childGetValue("seconds_in_chat_and_ims_check"));

	//Performance ----------------------------------------------------------------------------
	gSavedSettings.setBOOL("FetchInventoryOnLogin",		childGetValue("fetch_inventory_on_login_check"));
	gSavedSettings.setBOOL("WindEnabled",				childGetValue("enable_wind"));
	//Missing "Enabled Clouds"
	gSavedSettings.setBOOL("SpeedRez",					childGetValue("speed_rez_check"));
	gSavedSettings.setU32("SpeedRezInterval",			childGetValue("speed_rez_interval").asReal());
	
	//Commandline ----------------------------------------------------------------------------
	//Missing "Use Command Line"
	gSavedSettings.setString("AscentCmdLinePos",		childGetValue("AscentCmdLinePos"));
	gSavedSettings.setString("AscentCmdLineGround",		childGetValue("AscentCmdLineGround"));
	gSavedSettings.setString("AscentCmdLineHeight",		childGetValue("AscentCmdLineHeight"));
	gSavedSettings.setString("AscentCmdLineTeleportHome",	childGetValue("AscentCmdLineTeleportHome"));
	gSavedSettings.setString("AscentCmdLineRezPlatform",	childGetValue("AscentCmdLineRezPlatform"));
	//Platform Size
	gSavedSettings.setString("AscentCmdLineCalc",		childGetValue("AscentCmdLineCalc"));
	gSavedSettings.setString("AscentCmdLineClearChat",	childGetValue("AscentCmdLineClearChat"));
		//-------------------------------------------------------------------------------------
	gSavedSettings.setString("AscentCmdLineDrawDistance",	childGetValue("AscentCmdLineDrawDistance"));
	gSavedSettings.setString("AscentCmdTeleportToCam",		childGetValue("AscentCmdTeleportToCam"));
	gSavedSettings.setString("AscentCmdLineKeyToName",		childGetValue("AscentCmdLineKeyToName"));
	gSavedSettings.setString("AscentCmdLineOfferTp",		childGetValue("AscentCmdLineOfferTp"));
	gSavedSettings.setString("AscentCmdLineMapTo",		childGetValue("AscentCmdLineMapTo"));
	gSavedSettings.setBOOL("AscentCmdLineMapToKeepPos",	childGetValue("AscentCmdLineMapToKeepPos"));
	gSavedSettings.setString("AscentCmdLineTP2",		childGetValue("AscentCmdLineTP2"));
	

	//Privacy --------------------------------------------------------------------------------
	gSavedSettings.setBOOL("BroadcastViewerEffects",	childGetValue("broadcast_viewer_effects"));
	gSavedSettings.setBOOL("DisablePointAtAndBeam",		childGetValue("disable_point_at_and_beams_check"));
	gSavedSettings.setBOOL("PrivateLookAt",				childGetValue("private_look_at_check"));
	gSavedSettings.setBOOL("RevokePermsOnStandUp",		childGetValue("revoke_perms_on_stand_up_check"));

	refreshValues();
}

//---------------------------------------------------------------------------

LLPrefsAscentSys::LLPrefsAscentSys()
:	impl(* new LLPrefsAscentSysImpl())
{
}

LLPrefsAscentSys::~LLPrefsAscentSys()
{
	delete &impl;
}

void LLPrefsAscentSys::apply()
{
	impl.apply();
}

void LLPrefsAscentSys::cancel()
{
	impl.cancel();
}

LLPanel* LLPrefsAscentSys::getPanel()
{
	return &impl;
}