/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIWindowPVRGuide.h"

#include "FileItem.h"
#include "GUIUserMessages.h"
#include "ServiceBroker.h"
#include "addons/Skin.h"
#include "dialogs/GUIDialogBusy.h"
#include "dialogs/GUIDialogContextMenu.h"
#include "dialogs/GUIDialogNumeric.h"
#include "guilib/GUIMessage.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "messaging/helpers/DialogHelper.h"
#include "pvr/PVRGUIActions.h"
#include "pvr/PVRManager.h"
#include "pvr/channels/PVRChannel.h"
#include "pvr/channels/PVRChannelGroup.h"
#include "pvr/channels/PVRChannelGroupsContainer.h"
#include "pvr/epg/EpgChannelData.h"
#include "pvr/epg/EpgContainer.h"
#include "pvr/epg/EpgInfoTag.h"
#include "pvr/recordings/PVRRecordings.h"
#include "pvr/timers/PVRTimers.h"
#include "pvr/windows/GUIEPGGridContainer.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "view/GUIViewState.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace KODI::MESSAGING;
using namespace PVR;

CGUIWindowPVRGuideBase::CGUIWindowPVRGuideBase(bool bRadio, int id, const std::string& xmlFile) :
  CGUIWindowPVRBase(bRadio, id, xmlFile),
  m_bChannelSelectionRestored(false),
  m_bFirstOpen(true)
{
  m_bRefreshTimelineItems = false;
  m_bSyncRefreshTimelineItems = false;
  CServiceBroker::GetPVRManager().EpgContainer().Events().Subscribe(static_cast<CGUIWindowPVRBase*>(this), &CGUIWindowPVRBase::Notify);
}

CGUIWindowPVRGuideBase::~CGUIWindowPVRGuideBase()
{
  CServiceBroker::GetPVRManager().EpgContainer().Events().Unsubscribe(this);

  m_bRefreshTimelineItems = false;
  m_bSyncRefreshTimelineItems = false;
  StopRefreshTimelineItemsThread();
}

CGUIEPGGridContainer* CGUIWindowPVRGuideBase::GetGridControl()
{
  return dynamic_cast<CGUIEPGGridContainer*>(GetControl(m_viewControl.GetCurrentControl()));
}

void CGUIWindowPVRGuideBase::InitEpgGridControl()
{
  CGUIEPGGridContainer* epgGridContainer = GetGridControl();
  if (epgGridContainer)
  {
    m_bChannelSelectionRestored = epgGridContainer->SetChannel(CServiceBroker::GetPVRManager().GUIActions()->GetSelectedItemPath(m_bRadio));
    epgGridContainer->GoToNow();
  }

  if (epgGridContainer && !epgGridContainer->HasData())
    m_bSyncRefreshTimelineItems = true; // force data update on first window open

  StartRefreshTimelineItemsThread();
}

void CGUIWindowPVRGuideBase::ClearData()
{
  {
    CSingleLock lock(m_critSection);
    m_cachedChannelGroup.reset();
    m_newTimeline.reset();
  }

  CGUIWindowPVRBase::ClearData();
}

void CGUIWindowPVRGuideBase::OnInitWindow()
{
  if (m_guiState)
    m_viewControl.SetCurrentView(m_guiState->GetViewAsControl(), false);

  if (InitChannelGroup()) // no channels -> lazy init
    InitEpgGridControl();

  CGUIWindowPVRBase::OnInitWindow();
}

void CGUIWindowPVRGuideBase::OnDeinitWindow(int nextWindowID)
{
  StopRefreshTimelineItemsThread();

  m_bChannelSelectionRestored = false;

  {
    CSingleLock lock(m_critSection);
    if (m_vecItems && !m_newTimeline)
    {
      // speedup: save a copy of current items for reuse when re-opening the window
      m_newTimeline.reset(new CFileItemList);
      m_newTimeline->Assign(*m_vecItems);
    }
  }

  CGUIDialog* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetDialog(WINDOW_DIALOG_PVR_GUIDE_CONTROLS);
  if (dialog && dialog->IsDialogRunning())
  {
    dialog->Close();
  }

  CGUIWindowPVRBase::OnDeinitWindow(nextWindowID);
}

void CGUIWindowPVRGuideBase::StartRefreshTimelineItemsThread()
{
  StopRefreshTimelineItemsThread();
  m_refreshTimelineItemsThread.reset(new CPVRRefreshTimelineItemsThread(this));
  m_refreshTimelineItemsThread->Create();
}

void CGUIWindowPVRGuideBase::StopRefreshTimelineItemsThread()
{
  if (m_refreshTimelineItemsThread)
    m_refreshTimelineItemsThread->Stop();
}

void CGUIWindowPVRGuideBase::NotifyEvent(const PVREvent& event)
{
  if (event == PVREvent::Epg ||
      event == PVREvent::EpgContainer ||
      event == PVREvent::ChannelGroupInvalidated ||
      event == PVREvent::ChannelGroup)
  {
    m_bRefreshTimelineItems = true;
    // no base class call => do async refresh
    return;
  }
  else if (event == PVREvent::ChannelPlaybackStopped)
  {
    if (m_guiState && m_guiState->GetSortMethod().sortBy == SortByLastPlayed)
    {
      // set dirty to force sync refresh
      m_bSyncRefreshTimelineItems = true;
    }
  }

  // do sync refresh if dirty
  CGUIWindowPVRBase::NotifyEvent(event);
}

void CGUIWindowPVRGuideBase::SetInvalid()
{
  CGUIEPGGridContainer* epgGridContainer = GetGridControl();
  if (epgGridContainer)
    epgGridContainer->SetInvalid();

  CGUIWindowPVRBase::SetInvalid();
}

void CGUIWindowPVRGuideBase::GetContextButtons(int itemNumber, CContextButtons& buttons)
{
  CGUIWindowPVRBase::GetContextButtons(itemNumber, buttons);

  buttons.Add(CONTEXT_BUTTON_NAVIGATE, 19326); // Navigate...
}

void CGUIWindowPVRGuideBase::UpdateSelectedItemPath()
{
  CGUIEPGGridContainer* epgGridContainer = GetGridControl();
  if (epgGridContainer)
  {
    std::shared_ptr<CPVRChannel> channel(epgGridContainer->GetSelectedChannel());
    if (channel)
      CServiceBroker::GetPVRManager().GUIActions()->SetSelectedItemPath(m_bRadio, channel->Path());
  }
}

void CGUIWindowPVRGuideBase::UpdateButtons(void)
{
  CGUIWindowPVRBase::UpdateButtons();

  SET_CONTROL_LABEL(CONTROL_LABEL_HEADER1, g_localizeStrings.Get(19032));
  SET_CONTROL_LABEL(CONTROL_LABEL_HEADER2, GetChannelGroup()->GroupName());
}

bool CGUIWindowPVRGuideBase::Update(const std::string& strDirectory, bool updateFilterPath /* = true */)
{
  if (m_bUpdating)
  {
    // Prevent concurrent updates. Instead, let the timeline items refresh thread pick it up later.
    m_bRefreshTimelineItems = true;
    return true;
  }

  bool bReturn = CGUIWindowPVRBase::Update(strDirectory, updateFilterPath);

  if (bReturn && !m_bChannelSelectionRestored)
  {
    CGUIEPGGridContainer* epgGridContainer = GetGridControl();
    if (epgGridContainer)
      m_bChannelSelectionRestored = epgGridContainer->SetChannel(CServiceBroker::GetPVRManager().GUIActions()->GetSelectedItemPath(m_bRadio));
  }

  return bReturn;
}

bool CGUIWindowPVRGuideBase::GetDirectory(const std::string& strDirectory, CFileItemList& items)
{
  {
    CSingleLock lock(m_critSection);

    if (m_cachedChannelGroup && *m_cachedChannelGroup != *GetChannelGroup())
    {
      // channel group change and not very first open of this window. force immediate update.
      m_bSyncRefreshTimelineItems = true;
    }
  }

  // never call DoRefresh with locked mutex!
  if (m_bSyncRefreshTimelineItems)
    m_refreshTimelineItemsThread->DoRefresh(true);

  {
    CSingleLock lock(m_critSection);

    // Note: no need to do anything if no new data available. items always contains previous data.
    if (m_newTimeline)
    {
      items.RemoveDiscCache(GetID());
      items.Assign(*m_newTimeline, false);
      m_newTimeline.reset();
    }
  }

  return true;
}

void CGUIWindowPVRGuideBase::FormatAndSort(CFileItemList& items)
{
  if (&items == m_vecItems)
  {
    // Speedup: Nothing to do here as sorting was already done in CGUIWindowPVRGuideBase::RefreshTimelineItems
    return;
  }
  CGUIWindowPVRBase::FormatAndSort(items);
}

CFileItemPtr CGUIWindowPVRGuideBase::GetCurrentListItem(int offset /*= 0*/)
{
  CFileItemPtr item = CGUIWindowPVRBase::GetCurrentListItem(offset);
  if (!item)
  {
    // EPG "gap" item selected?
    CGUIEPGGridContainer* epgGridContainer = GetGridControl();
    if (epgGridContainer)
      item = epgGridContainer->GetSelectedGridItem(offset);
  }
  return item;
}

bool CGUIWindowPVRGuideBase::ShouldNavigateToGridContainer(int iAction)
{
  CGUIEPGGridContainer* epgGridContainer = GetGridControl();
  CGUIControl* control = GetControl(CONTROL_LSTCHANNELGROUPS);
  if (epgGridContainer && control &&
      GetFocusedControlID() == control->GetID())
  {
    int iNavigationId = control->GetAction(iAction).GetNavigation();
    if (iNavigationId > 0)
    {
      control = epgGridContainer;
      while (control != this) // navigation target could be the grid control or one of its parent controls.
      {
        if (iNavigationId == control->GetID())
        {
          // channel group selector control's target for the action is the grid control
          return true;
        }
        control = control->GetParentControl();
      }
    }
  }
  return false;
}

bool CGUIWindowPVRGuideBase::OnAction(const CAction& action)
{
  switch (action.GetID())
  {
    case ACTION_MOVE_UP:
    case ACTION_MOVE_DOWN:
    case ACTION_MOVE_LEFT:
    case ACTION_MOVE_RIGHT:
    {
      // Check whether grid container is configured as channel group selector's navigation target for the given action.
      if (ShouldNavigateToGridContainer(action.GetID()))
      {
        CGUIEPGGridContainer* epgGridContainer = GetGridControl();
        if (epgGridContainer)
        {
          CGUIWindowPVRBase::OnAction(action);

          switch (action.GetID())
          {
            case ACTION_MOVE_UP:
              epgGridContainer->GoToBottom();
              return true;
            case ACTION_MOVE_DOWN:
              epgGridContainer->GoToTop();
              return true;
            case ACTION_MOVE_LEFT:
              epgGridContainer->GoToMostRight();
              return true;
            case ACTION_MOVE_RIGHT:
              epgGridContainer->GoToMostLeft();
              return true;
            default:
              break;
          }
        }
      }
      break;
    }
    case REMOTE_0:
      if (GetCurrentDigitCount() == 0)
      {
        // single zero input is handled by epg grid container
        break;
      }
      // fall-thru is intended
    case REMOTE_1:
    case REMOTE_2:
    case REMOTE_3:
    case REMOTE_4:
    case REMOTE_5:
    case REMOTE_6:
    case REMOTE_7:
    case REMOTE_8:
    case REMOTE_9:
      AppendChannelNumberCharacter((action.GetID() - REMOTE_0) + '0');
      return true;

    case ACTION_CHANNEL_NUMBER_SEP:
      AppendChannelNumberCharacter(CPVRChannelNumber::SEPARATOR);
      return true;
  }

  return CGUIWindowPVRBase::OnAction(action);
}

void CGUIWindowPVRGuideBase::RefreshView(CGUIMessage& message, bool bInitGridControl)
{
  CGUIWindowPVRBase::OnMessage(message);

  // force grid data update
  m_bSyncRefreshTimelineItems = true;

  if (bInitGridControl)
    InitEpgGridControl();

  Refresh(true);
}

bool CGUIWindowPVRGuideBase::OnMessage(CGUIMessage& message)
{
  bool bReturn = false;
  switch (message.GetMessage())
  {
    case GUI_MSG_WINDOW_INIT:
      // if a path to a channel group is given we must init that group instead of last played/selected group
      m_channelGroupPath = message.GetStringParam(0);
      break;

    case GUI_MSG_CLICKED:
    {
      if (message.GetSenderId() == m_viewControl.GetCurrentControl())
      {
        if (message.GetParam1() == ACTION_SELECT_ITEM ||
            message.GetParam1() == ACTION_MOUSE_LEFT_CLICK)
        {
          // If direct channel number input is active, select the entered channel.
          if (CServiceBroker::GetPVRManager().GUIActions()->GetChannelNumberInputHandler().CheckInputAndExecuteAction())
          {
            bReturn = true;
            break;
          }
        }

        int iItem = m_viewControl.GetSelectedItem();
        if (iItem >= 0 && iItem < m_vecItems->Size())
        {
          CFileItemPtr pItem = m_vecItems->Get(iItem);
          /* process actions */
          switch (message.GetParam1())
          {
            case ACTION_SELECT_ITEM:
            case ACTION_MOUSE_LEFT_CLICK:
              switch(CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_EPG_SELECTACTION))
              {
                case EPG_SELECT_ACTION_CONTEXT_MENU:
                  OnPopupMenu(iItem);
                  bReturn = true;
                  break;
                case EPG_SELECT_ACTION_SWITCH:
                  CServiceBroker::GetPVRManager().GUIActions()->SwitchToChannel(pItem, true);
                  bReturn = true;
                  break;
                case EPG_SELECT_ACTION_PLAY_RECORDING:
                  CServiceBroker::GetPVRManager().GUIActions()->PlayRecording(pItem, true);
                  bReturn = true;
                  break;
                case EPG_SELECT_ACTION_INFO:
                  CServiceBroker::GetPVRManager().GUIActions()->ShowEPGInfo(pItem);
                  bReturn = true;
                  break;
                case EPG_SELECT_ACTION_RECORD:
                  CServiceBroker::GetPVRManager().GUIActions()->ToggleTimer(pItem);
                  bReturn = true;
                  break;
                case EPG_SELECT_ACTION_SMART_SELECT:
                {
                  const std::shared_ptr<CPVREpgInfoTag> tag(pItem->GetEPGInfoTag());
                  if (tag)
                  {
                    const CDateTime start(tag->StartAsUTC());
                    const CDateTime end(tag->EndAsUTC());
                    const CDateTime now(CDateTime::GetUTCDateTime());

                    if (start <= now && now <= end)
                    {
                      // current event
                      CServiceBroker::GetPVRManager().GUIActions()->SwitchToChannel(pItem, true);
                    }
                    else if (now < start)
                    {
                      // future event
                      if (CServiceBroker::GetPVRManager().Timers()->GetTimerForEpgTag(tag))
                        CServiceBroker::GetPVRManager().GUIActions()->EditTimer(pItem);
                      else
                      {
                        HELPERS::DialogResponse ret
                          = HELPERS::ShowYesNoDialogText(CVariant{19096}, // "Smart select"
                                                          CVariant{19302}, // "Do you want to record the selected programme or to switch to the current programme?"
                                                          CVariant{264}, // No => "Record"
                                                          CVariant{19165}); // Yes => "Switch"
                        if (ret == HELPERS::DialogResponse::NO)
                          CServiceBroker::GetPVRManager().GUIActions()->AddTimer(pItem, false);
                        else if (ret == HELPERS::DialogResponse::YES)
                          CServiceBroker::GetPVRManager().GUIActions()->SwitchToChannel(pItem, true);
                      }
                    }
                    else
                    {
                      // past event
                      if (CServiceBroker::GetPVRManager().Recordings()->GetRecordingForEpgTag(tag))
                        CServiceBroker::GetPVRManager().GUIActions()->PlayRecording(pItem, true);
                      else if (tag->IsPlayable())
                        CServiceBroker::GetPVRManager().GUIActions()->PlayEpgTag(pItem);
                      else
                        CServiceBroker::GetPVRManager().GUIActions()->ShowEPGInfo(pItem);
                    }
                    bReturn = true;
                  }
                  break;
                }
              }
              break;
            case ACTION_SHOW_INFO:
              CServiceBroker::GetPVRManager().GUIActions()->ShowEPGInfo(pItem);
              bReturn = true;
              break;
            case ACTION_PLAYER_PLAY:
              CServiceBroker::GetPVRManager().GUIActions()->SwitchToChannel(pItem, true);
              bReturn = true;
              break;
            case ACTION_RECORD:
              CServiceBroker::GetPVRManager().GUIActions()->ToggleTimer(pItem);
              bReturn = true;
              break;
            case ACTION_PVR_SHOW_TIMER_RULE:
              CServiceBroker::GetPVRManager().GUIActions()->AddTimerRule(pItem, true, false);
              bReturn = true;
              break;
            case ACTION_CONTEXT_MENU:
            case ACTION_MOUSE_RIGHT_CLICK:
              OnPopupMenu(iItem);
              bReturn = true;
              break;
          }
        }
        else if (iItem == -1)
        {
          /* process actions */
          switch (message.GetParam1())
          {
            case ACTION_SELECT_ITEM:
            case ACTION_MOUSE_LEFT_CLICK:
            case ACTION_PLAYER_PLAY:
            {
              // EPG "gap" selected => switch to associated channel.
              CGUIEPGGridContainer* epgGridContainer = GetGridControl();
              if (epgGridContainer)
              {
                const CFileItemPtr item(epgGridContainer->GetSelectedGridItem());
                if (item)
                {
                  CServiceBroker::GetPVRManager().GUIActions()->SwitchToChannel(item, true);
                  bReturn = true;
                }
              }
              break;
            }
            case ACTION_CONTEXT_MENU:
            {
              // EPG "gap" selected => create and process special context menu with item independent entries.
              CContextButtons buttons;
              GetContextButtons(-1, buttons);

              int iButton = CGUIDialogContextMenu::ShowAndGetChoice(buttons);
              if (iButton >= 0)
              {
                bReturn = OnContextButton(-1, static_cast<CONTEXT_BUTTON>(iButton));
              }
              break;
            }
          }
        }
      }
      else if (message.GetSenderId() == CONTROL_BTNVIEWASICONS ||
               message.GetSenderId() == CONTROL_BTNSORTBY)
      {
        RefreshView(message, false);
        bReturn = true;
      }
      break;
    }
    case GUI_MSG_CHANGE_SORT_DIRECTION:
    case GUI_MSG_CHANGE_SORT_METHOD:
    case GUI_MSG_CHANGE_VIEW_MODE:
    {
      RefreshView(message, message.GetMessage() == GUI_MSG_CHANGE_VIEW_MODE);
      bReturn = true;
      break;
    }
    case GUI_MSG_REFRESH_LIST:
    {
      switch (static_cast<PVREvent>(message.GetParam1()))
      {
        case PVREvent::ChannelGroupsLoaded:
          // late init
          InitChannelGroup();
          InitEpgGridControl();
          break;

        case PVREvent::ChannelGroup:
        case PVREvent::ChannelGroupInvalidated:
        case PVREvent::ChannelPlaybackStopped:
        case PVREvent::Epg:
        case PVREvent::EpgContainer:
          Refresh(true);
          break;

        case PVREvent::Timers:
        case PVREvent::TimersInvalidated:
          SetInvalid();
          break;

        default:
          break;
      }
      break;
    }
  }

  return bReturn || CGUIWindowPVRBase::OnMessage(message);
}

bool CGUIWindowPVRGuideBase::OnContextButton(int itemNumber, CONTEXT_BUTTON button)
{
  if (OnContextButtonNavigate(button))
    return true;

  if (itemNumber < 0 || itemNumber >= m_vecItems->Size())
    return false;

  return CGUIMediaWindow::OnContextButton(itemNumber, button);
}

namespace
{

template<typename A>
class CContextMenuFunctions : public CContextButtons
{
public:
  explicit CContextMenuFunctions(A* instance) : m_instance(instance) {}

  void Add(bool (A::*function)(), unsigned int resId)
  {
    CContextButtons::Add(size(), resId);
    m_functions.emplace_back(std::bind(function, m_instance));
  }

  bool Call(int idx)
  {
    if (idx < 0 || idx >= static_cast<int>(m_functions.size()))
      return false;

    return m_functions[idx]();
  }

private:
  A* m_instance = nullptr;
  std::vector<std::function<bool()>> m_functions;
};

} // unnamed namespace

bool CGUIWindowPVRGuideBase::OnContextButtonNavigate(CONTEXT_BUTTON button)
{
  bool bReturn = false;

  if (button == CONTEXT_BUTTON_NAVIGATE)
  {
    if (g_SkinInfo->HasSkinFile("DialogPVRGuideControls.xml"))
    {
      // use controls dialog
      CGUIDialog* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetDialog(WINDOW_DIALOG_PVR_GUIDE_CONTROLS);
      if (dialog && !dialog->IsDialogRunning())
      {
        dialog->Open();
      }
    }
    else
    {
      // use context menu
      CContextMenuFunctions<CGUIWindowPVRGuideBase> buttons(this);
      buttons.Add(&CGUIWindowPVRGuideBase::GotoBegin, 19063); // First programme
      buttons.Add(&CGUIWindowPVRGuideBase::Go12HoursBack, 19317); // 12 hours back
      buttons.Add(&CGUIWindowPVRGuideBase::GotoNow, 19070); // Current programme
      buttons.Add(&CGUIWindowPVRGuideBase::Go12HoursForward, 19318); // 12 hours forward
      buttons.Add(&CGUIWindowPVRGuideBase::GotoEnd, 19064); // Last programme
      buttons.Add(&CGUIWindowPVRGuideBase::OpenDateSelectionDialog, 19288); // Date selector
      buttons.Add(&CGUIWindowPVRGuideBase::GotoFirstChannel, 19322); // First channel
      if (CServiceBroker::GetPVRManager().IsPlayingTV() || CServiceBroker::GetPVRManager().IsPlayingRadio())
        buttons.Add(&CGUIWindowPVRGuideBase::GotoPlayingChannel, 19323); // Playing channel
      buttons.Add(&CGUIWindowPVRGuideBase::GotoLastChannel, 19324); // Last channel
      buttons.Add(&CGUIWindowPVRBase::ActivatePreviousChannelGroup, 19319); // Previous group
      buttons.Add(&CGUIWindowPVRBase::ActivateNextChannelGroup, 19320); // Next group
      buttons.Add(&CGUIWindowPVRBase::OpenChannelGroupSelectionDialog, 19321); // Group selector

      int buttonIdx = 0;
      int lastButtonIdx = 2; // initially select "Current programme"

      // loop until canceled
      while (buttonIdx >= 0)
      {
        buttonIdx = CGUIDialogContextMenu::Show(buttons, lastButtonIdx);
        lastButtonIdx = buttonIdx;
        buttons.Call(buttonIdx);
      }
    }
    bReturn = true;
  }

  return bReturn;
}

bool CGUIWindowPVRGuideBase::RefreshTimelineItems()
{
  if (m_bRefreshTimelineItems || m_bSyncRefreshTimelineItems)
  {
    m_bRefreshTimelineItems = false;
    m_bSyncRefreshTimelineItems = false;

    CGUIEPGGridContainer* epgGridContainer = GetGridControl();
    if (epgGridContainer)
    {
      const std::shared_ptr<CPVRChannelGroup> group(GetChannelGroup());
      if (!group)
        return false;

      std::unique_ptr<CFileItemList> timeline(new CFileItemList);

      if (m_bFirstOpen)
      {
        m_bFirstOpen = false;

        // very first open of the window. come up with some data very fast...
        const std::vector<PVRChannelGroupMember> groupMembers = group->GetMembers(CPVRChannelGroup::Include::ONLY_VISIBLE);
        for (const auto& groupMember : groupMembers)
        {
          // fake a channel without epg

          const std::shared_ptr<CPVREpgInfoTag> gapTag
            = std::make_shared<CPVREpgInfoTag>(std::make_shared<CPVREpgChannelData>(*(groupMember.channel)), -1);
          timeline->Add(std::make_shared<CFileItem>(gapTag));
        }

        // next, fetch actual data.
        m_bRefreshTimelineItems = true;
        m_refreshTimelineItemsThread->DoRefresh(false);
      }
      else
      {
        // can be very expensive. never call with lock acquired.
        const std::vector<std::shared_ptr<CPVREpgInfoTag>> tags = group->GetEPGAll(true);
        for (const auto& tag : tags)
        {
          timeline->Add(std::make_shared<CFileItem>(tag));
        }
      }

      CDateTime startDate(group->GetFirstEPGDate());
      CDateTime endDate(group->GetLastEPGDate());
      const CDateTime currentDate(CDateTime::GetCurrentDateTime().GetAsUTCDateTime());

      if (!startDate.IsValid())
        startDate = currentDate;

      if (!endDate.IsValid() || endDate < startDate)
        endDate = startDate;

      CPVREpgContainer& epgContainer = CServiceBroker::GetPVRManager().EpgContainer();

      // limit start to past days to display
      int iPastDays = epgContainer.GetPastDaysToDisplay();
      const CDateTime maxPastDate(currentDate - CDateTimeSpan(iPastDays, 0, 0, 0));
      if (startDate < maxPastDate)
        startDate = maxPastDate;

      // limit end to future days to display
      int iFutureDays = epgContainer.GetFutureDaysToDisplay();
      const CDateTime maxFutureDate(currentDate + CDateTimeSpan(iFutureDays, 0, 0, 0));
      if (endDate > maxFutureDate)
        endDate = maxFutureDate;

      if (m_guiState)
        timeline->Sort(m_guiState->GetSortMethod());

      // can be very expensive. never call with lock acquired.
      epgGridContainer->SetTimelineItems(timeline, startDate, endDate);

      {
        CSingleLock lock(m_critSection);

        m_newTimeline = std::move(timeline);
        m_cachedChannelGroup = group;
      }
      return true;
    }
  }
  return false;
}

bool CGUIWindowPVRGuideBase::GotoBegin()
{
  GetGridControl()->GoToBegin();
  return true;
}

bool CGUIWindowPVRGuideBase::GotoEnd()
{
  GetGridControl()->GoToEnd();
  return true;
}

bool CGUIWindowPVRGuideBase::GotoNow()
{
  GetGridControl()->GoToNow();
  return true;
}

bool CGUIWindowPVRGuideBase::OpenDateSelectionDialog()
{
  bool bReturn = false;

  SYSTEMTIME date;
  CGUIEPGGridContainer* epgGridContainer = GetGridControl();
  epgGridContainer->GetSelectedDate().GetAsSystemTime(date);

  if (CGUIDialogNumeric::ShowAndGetDate(date, g_localizeStrings.Get(19288))) /* Go to date */
  {
    epgGridContainer->GoToDate(CDateTime(date));
    bReturn = true;
  }

  return bReturn;
}

bool CGUIWindowPVRGuideBase::Go12HoursBack()
{
  return GotoDate(-12);
}

bool CGUIWindowPVRGuideBase::Go12HoursForward()
{
  return GotoDate(+12);
}

bool CGUIWindowPVRGuideBase::GotoDate(int deltaHours)
{
  CGUIEPGGridContainer* epgGridContainer = GetGridControl();
  epgGridContainer->GoToDate(epgGridContainer->GetSelectedDate() + CDateTimeSpan(0, deltaHours, 0, 0));
  return true;
}

bool CGUIWindowPVRGuideBase::GotoFirstChannel()
{
  GetGridControl()->GoToFirstChannel();
  return true;
}

bool CGUIWindowPVRGuideBase::GotoLastChannel()
{
  GetGridControl()->GoToLastChannel();
  return true;
}

bool CGUIWindowPVRGuideBase::GotoPlayingChannel()
{
  const std::shared_ptr<CPVRChannel> channel = CServiceBroker::GetPVRManager().GetPlayingChannel();
  if (channel)
  {
    GetGridControl()->SetChannel(channel);
    return true;
  }
  return false;
}

void CGUIWindowPVRGuideBase::OnInputDone()
{
  const CPVRChannelNumber channelNumber = GetChannelNumber();
  if (channelNumber.IsValid())
  {
    CGUIEPGGridContainer* epgGridContainer = GetGridControl();
    if (epgGridContainer)
    {
      for (const std::shared_ptr<CFileItem>& event : *m_vecItems)
      {
        const std::shared_ptr<CPVRChannel> channel = CServiceBroker::GetPVRManager().ChannelGroups()->GetChannelForEpgTag(event->GetEPGInfoTag());
        if (channel && channel->ChannelNumber() == channelNumber)
        {
          epgGridContainer->SetChannel(channel);
          return;
        }
      }
    }
  }
}

void CGUIWindowPVRGuideBase::GetChannelNumbers(std::vector<std::string>& channelNumbers)
{
  const std::shared_ptr<CPVRChannelGroup> group = GetChannelGroup();
  if (group)
    group->GetChannelNumbers(channelNumbers);
}

CPVRRefreshTimelineItemsThread::CPVRRefreshTimelineItemsThread(CGUIWindowPVRGuideBase* pGuideWindow)
: CThread("epg-grid-refresh-timeline-items"),
  m_pGuideWindow(pGuideWindow),
  m_ready(true),
  m_done(false)
{
}

CPVRRefreshTimelineItemsThread::~CPVRRefreshTimelineItemsThread()
{
  // Note: CThread dtor will also call StopThread(true), but if thread worker function exits that
  //       late, it might access member variables of this which are already destroyed. Thus, stop
  //       the thread worker here and synchronously, while all members of this are still alive.
  StopThread(true);
}

void CPVRRefreshTimelineItemsThread::Stop()
{
  StopThread(false);
  m_ready.Set(); // wake up the worker thread to let it exit
}

void CPVRRefreshTimelineItemsThread::DoRefresh(bool bWait)
{
  m_ready.Set(); // wake up the worker thread

  if (bWait)
  {
    m_done.Reset();
    CGUIDialogBusy::WaitOnEvent(m_done, 100, false);
  }
}

void CPVRRefreshTimelineItemsThread::Process()
{
  static const int BOOSTED_SLEEPS_THRESHOLD = 4;

  int iLastEpgItemsCount = 0;
  int iUpdatesWithoutChange = 0;

  while (!m_bStop)
  {
    m_done.Reset();

    if (m_pGuideWindow->RefreshTimelineItems() && !m_bStop)
    {
      CGUIMessage m(GUI_MSG_REFRESH_LIST, m_pGuideWindow->GetID(), 0, static_cast<int>(PVREvent::Epg));
      KODI::MESSAGING::CApplicationMessenger::GetInstance().SendGUIMessage(m);
    }

    if (m_bStop)
      break;

    m_done.Set();

    // in order to fill the guide window asap, use a short update interval until we the
    // same amount of epg events for BOOSTED_SLEEPS_THRESHOLD + 1 times in a row .
    if (iUpdatesWithoutChange < BOOSTED_SLEEPS_THRESHOLD)
    {
      int iCurrentEpgItemsCount = m_pGuideWindow->CurrentDirectory().Size();

      if (iCurrentEpgItemsCount == iLastEpgItemsCount)
        iUpdatesWithoutChange++;
      else
        iUpdatesWithoutChange = 0; // reset

      iLastEpgItemsCount = iCurrentEpgItemsCount;

      m_ready.WaitMSec(1000); // boosted update cycle
    }
    else
    {
      m_ready.WaitMSec(5000); // normal update cycle
    }

    m_ready.Reset();
  }

  m_ready.Reset();
  m_done.Set();
}
