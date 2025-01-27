/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "addons/kodi-addon-dev-kit/include/kodi/xbmc_pvr_types.h"
#include "interfaces/IAnnouncer.h"
#include "pvr/PVRActionListener.h"
#include "pvr/PVRSettings.h"
#include "pvr/epg/EpgContainer.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/Thread.h"
#include "utils/EventStream.h"

#include <memory>
#include <string>
#include <vector>

class CFileItem;
class CJob;
class CStopWatch;

namespace PVR
{
  class CPVRChannel;
  class CPVRChannelGroup;
  class CPVRChannelGroupsContainer;
  class CPVRClient;
  class CPVRClients;
  class CPVRDatabase;
  class CPVRGUIActions;
  class CPVRGUIInfo;
  class CPVRGUIProgressHandler;
  class CPVRRecording;
  class CPVRRecordings;
  class CPVRTimers;

  enum class PVREvent
  {
    // PVR Manager states
    ManagerError = 0,
    ManagerStopped,
    ManagerStarting,
    ManagerStopping,
    ManagerInterrupted,
    ManagerStarted,

    // Channel events
    ChannelPlaybackStopped,

    // Channel group events
    ChannelGroup,
    ChannelGroupInvalidated,
    ChannelGroupsInvalidated,
    ChannelGroupsLoaded,

    // Recording events
    RecordingsInvalidated,

    // Timer events
    AnnounceReminder,
    Timers,
    TimersInvalidated,

    // EPG events
    Epg,
    EpgActiveItem,
    EpgContainer,
    EpgItemUpdate,
    EpgUpdatePending,

    // Item events
    CurrentItem,
  };

  class CPVRManagerJobQueue
  {
  public:
    CPVRManagerJobQueue();

    void Start();
    void Stop();
    void Clear();

    void AppendJob(CJob * job);
    void ExecutePendingJobs();
    bool WaitForJobs(unsigned int milliSeconds);

  private:
    CCriticalSection m_critSection;
    CEvent m_triggerEvent;
    std::vector<CJob *> m_pendingUpdates;
    bool m_bStopped = true;
  };

  class CPVRManager : private CThread, public ANNOUNCEMENT::IAnnouncer
  {
  public:
    /*!
     * @brief Create a new CPVRManager instance, which handles all PVR related operations in XBMC.
     */
    CPVRManager(void);

    /*!
     * @brief Stop the PVRManager and destroy all objects it created.
     */
    ~CPVRManager(void) override;

    void Announce(ANNOUNCEMENT::AnnouncementFlag flag, const char* sender, const char* message, const CVariant& data) override;

    /*!
     * @brief Get the channel groups container.
     * @return The groups container.
     */
    std::shared_ptr<CPVRChannelGroupsContainer> ChannelGroups(void) const;

    /*!
     * @brief Get the recordings container.
     * @return The recordings container.
     */
    std::shared_ptr<CPVRRecordings> Recordings(void) const;

    /*!
     * @brief Get the timers container.
     * @return The timers container.
     */
    std::shared_ptr<CPVRTimers> Timers(void) const;

    /*!
     * @brief Get the timers container.
     * @return The timers container.
     */
    std::shared_ptr<CPVRClients> Clients(void) const;

    /*!
     * @brief Get the instance of a client that matches the given item.
     * @param item The item containing a PVR recording, a PVR channel, a PVR timer or a PVR EPG event.
     * @return the requested client on success, nullptr otherwise.
     */
    std::shared_ptr<CPVRClient> GetClient(const CFileItem& item) const;

    /*!
     * @brief Get the instance of a client that matches the given id.
     * @param iClientId The id of a PVR client.
     * @return the requested client on success, nullptr otherwise.
     */
    std::shared_ptr<CPVRClient> GetClient(int iClientId) const;

    /*!
     * @brief Get access to the pvr gui actions.
     * @return The gui actions.
     */
    std::shared_ptr<CPVRGUIActions> GUIActions(void) const;

    /*!
     * @brief Get access to the epg container.
     * @return The epg container.
     */
    CPVREpgContainer& EpgContainer();

    /*!
     * @brief Init PVRManager.
     */
    void Init(void);

    /*!
     * @brief Start the PVRManager, which loads all PVR data and starts some threads to update the PVR data.
     */
    void Start();

    /*!
     * @brief Stop PVRManager.
     */
    void Stop(void);

    /*!
     * @brief Stop PVRManager, unload data.
     */
    void Unload();

    /*!
     * @brief Deinit PVRManager, unload data, unload addons.
     */
    void Deinit();

    /*!
     * @brief Propagate event on system sleep
     */
    void OnSleep();

    /*!
     * @brief Propagate event on system wake
     */
    void OnWake();

    /*!
     * @brief Get the TV database.
     * @return The TV database.
     */
    std::shared_ptr<CPVRDatabase> GetTVDatabase(void) const;

    /*!
     * @brief Check if a TV channel, radio channel or recording is playing.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlaying(void) const;

    /*!
     * @brief Check if the given channel is playing.
     * @param channel The channel to check.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingChannel(const std::shared_ptr<CPVRChannel>& channel) const;

    /*!
     * @brief Check if the given recording is playing.
     * @param recording The recording to check.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingRecording(const std::shared_ptr<CPVRRecording>& recording) const;

    /*!
     * @brief Check if the given epg tag is playing.
     * @param epgTag The tag to check.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingEpgTag(const std::shared_ptr<CPVREpgInfoTag>& epgTag) const;

    /*!
     * @return True while the PVRManager is initialising.
     */
    inline bool IsInitialising(void) const
    {
      return GetState() == ManagerStateStarting;
    }

    /*!
     * @brief Check whether the PVRManager has fully started.
     * @return True if started, false otherwise.
     */
    inline bool IsStarted(void) const
    {
      return GetState() == ManagerStateStarted;
    }

    /*!
     * @brief Check whether the PVRManager is stopping
     * @return True while the PVRManager is stopping.
     */
    inline bool IsStopping(void) const
    {
      return GetState() == ManagerStateStopping;
    }

    /*!
     * @brief Check whether the PVRManager has been stopped.
     * @return True if stopped, false otherwise.
     */
    inline bool IsStopped(void) const
    {
      return GetState() == ManagerStateStopped;
    }

    /*!
     * @brief Check whether playing channel matches given uids.
     * @param iClientID The client id.
     * @param iUniqueChannelID The channel uid.
     * @return True on match, false if there is no match or no channel is playing.
     */
    bool IsPlayingChannel(int iClientID, int iUniqueChannelID) const;

    /*!
     * @brief Return the channel that is currently playing.
     * @return The channel or NULL if none is playing.
     */
    std::shared_ptr<CPVRChannel> GetPlayingChannel(void) const;

    /*!
     * @brief Return the recording that is currently playing.
     * @return The recording or NULL if none is playing.
     */
    std::shared_ptr<CPVRRecording> GetPlayingRecording(void) const;

    /*!
     * @brief Return the epg tag that is currently playing.
     * @return The tag or NULL if none is playing.
     */
    std::shared_ptr<CPVREpgInfoTag> GetPlayingEpgTag(void) const;

    /*!
     * @brief Get the name of the playing client, if there is one.
     * @return The name of the client or an empty string if nothing is playing.
     */
    std::string GetPlayingClientName(void) const;

    /*!
     * @brief Get the ID of the playing client, if there is one.
     * @return The ID or -1 if no client is playing.
     */
    int GetPlayingClientID(void) const;

    /*!
     * @brief Check whether there is an active recording on the currenlyt playing channel.
     * @return True if there is a playing channel and there is an active recording on that channel, false otherwise.
     */
    bool IsRecordingOnPlayingChannel(void) const;

    /*!
     * @brief Check if an active recording is playing.
     * @return True if an in-progress (active) recording is playing, false otherwise.
     */
    bool IsPlayingActiveRecording() const;

    /*!
     * @brief Check whether the currently playing channel can be recorded.
     * @return True if there is a playing channel that can be recorded, false otherwise.
     */
    bool CanRecordOnPlayingChannel(void) const;

    /*!
     * @brief Check whether EPG tags for channels have been created.
     * @return True if EPG tags have been created, false otherwise.
     */
    bool EpgsCreated(void) const;

    /*!
     * @brief Inform PVR manager that playback of an item just started.
     * @param item The item that started to play.
     */
    void OnPlaybackStarted(const std::shared_ptr<CFileItem> item);

    /*!
     * @brief Inform PVR manager that playback of an item was stopped due to user interaction.
     * @param item The item that stopped to play.
     */
    void OnPlaybackStopped(const std::shared_ptr<CFileItem> item);

    /*!
     * @brief Inform PVR manager that playback of an item has stopped without user interaction.
     * @param item The item that ended to play.
     */
    void OnPlaybackEnded(const std::shared_ptr<CFileItem> item);

    /*!
     * @brief Check whether there are active recordings.
     * @return True if there are active recordings, false otherwise.
     */
    bool IsRecording(void) const;

    /*!
     * @brief Set the current playing group, used to load the right channel.
     * @param group The new group.
     */
    void SetPlayingGroup(const std::shared_ptr<CPVRChannelGroup>& group);

    /*!
     * @brief Get the current playing group, used to load the right channel.
     * @param bRadio True to get the current radio group, false to get the current TV group.
     * @return The current group or the group containing all channels if it's not set.
     */
    std::shared_ptr<CPVRChannelGroup> GetPlayingGroup(bool bRadio = false) const;

    /*!
     * @brief Let the background thread create epg tags for all channels.
     */
    void TriggerEpgsCreate(void);

    /*!
     * @brief Let the background thread update the recordings list.
     */
    void TriggerRecordingsUpdate(void);

    /*!
     * @brief Let the background thread update the timer list.
     */
    void TriggerTimersUpdate(void);

    /*!
     * @brief Let the background thread update the channel list.
     */
    void TriggerChannelsUpdate(void);

    /*!
     * @brief Let the background thread update the channel groups list.
     */
    void TriggerChannelGroupsUpdate(void);

    /*!
     * @brief Let the background thread search for all missing channel icons.
     */
    void TriggerSearchMissingChannelIcons(void);

    /*!
     * @brief Let the background thread search for missing channel icons for channels contained in the given group.
     * @param group The channel group.
     */
    void TriggerSearchMissingChannelIcons(const std::shared_ptr<CPVRChannelGroup>& group);

    /*!
     * @brief Check whether names are still correct after the language settings changed.
     */
    void LocalizationChanged(void);

    /*!
     * @brief Check if a TV channel is playing.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingTV(void) const;

    /*!
     * @brief Check if a radio channel is playing.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingRadio(void) const;

    /*!
     * @brief Check if a an encrypted TV or radio channel is playing.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingEncryptedChannel(void) const;

    /*!
     * @brief Check if a recording is playing.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingRecording(void) const;

    /*!
     * @brief Check if an epg tag is playing.
     * @return True if it's playing, false otherwise.
     */
    bool IsPlayingEpgTag(void) const;

    /*!
     * @brief Check if parental lock is overridden at the given moment.
     * @param channel The channel to check.
     * @return True if parental lock is overridden, false otherwise.
     */
    bool IsParentalLocked(const std::shared_ptr<CPVRChannel>& channel) const;

    /*!
     * @brief Check if parental lock is overridden at the given moment.
     * @param epgTag The epg tag to check.
     * @return True if parental lock is overridden, false otherwise.
     */
    bool IsParentalLocked(const std::shared_ptr<CPVREpgInfoTag>& epgTag) const;

    /*!
     * @brief Restart the parental timer.
     */
    void RestartParentalTimer();

    /*!
     * @brief Create EPG tags for all channels in internal channel groups
     * @return True if EPG tags where created successfully, false otherwise
     */
    bool CreateChannelEpgs(void);

    /*!
     * @brief Signal a connection change of a client
     */
    void ConnectionStateChange(CPVRClient* client, std::string connectString, PVR_CONNECTION_STATE state, std::string message);

    /*!
     * @brief Query the events available for CEventStream
     */
    CEventStream<PVREvent>& Events() { return m_events; }

    /*!
     * @brief Publish an event
     * @param state the event
     */
    void PublishEvent(PVREvent state);

  protected:
    /*!
     * @brief PVR update and control thread.
     */
    void Process(void) override;

  private:
    /*!
     * @brief Updates the last watched timestamps of the channel and group which are currently playing.
     * @param channel The channel which is updated
     * @param time The last watched time to set
     */
    void UpdateLastWatched(const std::shared_ptr<CPVRChannel>& channel, const CDateTime& time);

    /*!
     * @brief Set the playing group to the first group the channel is in if the given channel is not part of the current playing group
     * @param channel The channel
     */
    void SetPlayingGroup(const std::shared_ptr<CPVRChannel>& channel);

    /*!
     * @brief Executes "pvrpowermanagement.setwakeupcmd"
     */
    bool SetWakeupCommand(void);

    /*!
     * @brief Load at least one client and load all other PVR data (channelgroups, timers, recordings) after loading the client.
     * @param progressHandler The progress handler to use for showing the different load stages.
     * @return If at least one client and all pvr data was loaded, false otherwise.
     */
    bool LoadComponents(CPVRGUIProgressHandler* progressHandler);

    /*!
     * @brief Unload all PVR data (recordings, timers, channelgroups).
     */
    void UnloadComponents();

    /*!
     * @brief Reset all properties.
     */
    void ResetProperties(void);

    /*!
     * @brief Destroy PVRManager's objects.
     */
    void Clear(void);

    /*!
     * @brief Continue playback on the last played channel.
     */
    void TriggerPlayChannelOnStartup(void);

    enum ManagerState
    {
      ManagerStateError = 0,
      ManagerStateStopped,
      ManagerStateStarting,
      ManagerStateStopping,
      ManagerStateInterrupted,
      ManagerStateStarted
    };

    /*!
     * @brief Get the current state of the PVR manager.
     * @return the state.
     */
    ManagerState GetState(void) const;

    /*!
     * @brief Set the current state of the PVR manager.
     * @param state the new state.
     */
    void SetState(ManagerState state);

    bool IsCurrentlyParentalLocked(const std::shared_ptr<CPVRChannel>& channel, bool bGenerallyLocked) const;

    /** @name containers */
    //@{
    std::shared_ptr<CPVRChannelGroupsContainer>  m_channelGroups;               /*!< pointer to the channel groups container */
    std::shared_ptr<CPVRRecordings>              m_recordings;                  /*!< pointer to the recordings container */
    std::shared_ptr<CPVRTimers>                  m_timers;                      /*!< pointer to the timers container */
    std::shared_ptr<CPVRClients>                 m_addons;                      /*!< pointer to the pvr addon container */
    std::unique_ptr<CPVRGUIInfo>   m_guiInfo;                     /*!< pointer to the guiinfo data */
    std::shared_ptr<CPVRGUIActions>              m_guiActions;                  /*!< pointer to the pvr gui actions */
    CPVREpgContainer               m_epgContainer;                /*!< the epg container */
    //@}

    CPVRManagerJobQueue             m_pendingUpdates;              /*!< vector of pending pvr updates */

    std::shared_ptr<CPVRDatabase>                 m_database;                    /*!< the database for all PVR related data */
    mutable CCriticalSection        m_critSection;                 /*!< critical section for all changes to this class, except for changes to triggers */
    bool                            m_bFirstStart = true;          /*!< true when the PVR manager was started first, false otherwise */
    bool                            m_bEpgsCreated = false;        /*!< true if epg data for channels has been created */

    mutable CCriticalSection        m_managerStateMutex;
    ManagerState                    m_managerState = ManagerStateStopped;
    std::unique_ptr<CStopWatch>     m_parentalTimer;

    CCriticalSection                m_startStopMutex; // mutex for protecting pvr manager's start/restart/stop sequence */

    CEventSource<PVREvent> m_events;

    CPVRActionListener m_actionListener;
    CPVRSettings m_settings;

    std::shared_ptr<CPVRChannel> m_playingChannel;
    std::shared_ptr<CPVRRecording> m_playingRecording;
    std::shared_ptr<CPVREpgInfoTag> m_playingEpgTag;
    std::string m_strPlayingClientName;
    int m_playingClientId = -1;
    int m_iplayingChannelUniqueID = -1;

    class CLastWatchedUpdateTimer;
    std::unique_ptr<CLastWatchedUpdateTimer> m_lastWatchedUpdateTimer;
  };
}
