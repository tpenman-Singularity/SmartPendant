//******************************************************************************
//  @file WrapJobScr.h
//  @brief Wire Wrap guided-job screen for SmartPendant (grblHAL)
//
//  Purpose: one-screen operator workflow for a 2-axis (X + A) wire winding
//  machine. The job sequence itself lives in a .nc file on the CONTROLLER's
//  SD card (Teensy 4.1 slot). The file contains M0 pauses with (MSG,...)
//  prompts. This screen:
//    - lists job files on the controller SD ($F)
//    - starts a job ($F=<name>)
//    - shows the current step prompt ([MSG:...] lines from grblHAL)
//    - shows big X / A-turns DRO and machine state
//    - drives the job with one big context button:
//        READY -> START JOB, RUN -> HOLD, HOLD -> CONTINUE,
//        DONE -> RUN AGAIN, ALARM -> UNLOCK
//
//  This file follows the structure of the stock SmartPendant screens
//  (ProbeScr / ProgramSender). If the IScr interface signatures in your
//  checkout differ, match them to whatever ProbeScr.h declares.
//******************************************************************************

#ifndef WrapJobScr_h
#define WrapJobScr_h

// *****************************************************************************
// ***   Includes   ************************************************************
// *****************************************************************************
#include "DevCfg.h"        // Project-wide config (display, colors, fonts)
#include "DisplayDrv.h"    // DevCore display driver (String, Box, ...)
#include "UiEngine.h"      // DevCore UI (UiButton). If the project keeps
                           // UiButton elsewhere, include that header instead.
#include "IScr.h"          // Screen interface used by all SmartPendant screens
#include "GrblComm.h"      // grblHAL communication task (singleton)

// *****************************************************************************
// ***   WrapJobScr   **********************************************************
// *****************************************************************************
class WrapJobScr : public IScr
{
  public:
    // *** Get singleton instance *********************************************
    static WrapJobScr& GetInstance();

    // *** Screen interface (match signatures used by ProbeScr) ***************
    virtual Result Setup(int32_t y, int32_t height);
    virtual Result Show();
    virtual Result Hide();
    virtual Result TimerExpired(uint32_t interval);
    virtual Result ProcessCallback(const void* ptr);

  private:
    // Max job files shown from the controller SD card
    static const uint32_t MAX_FILES = 10u;
    // Max stored filename length ("/WRAP01.NC" style - keep names short)
    static const uint32_t MAX_FNAME = 28u;
    // Step prompt: two display lines
    static const uint32_t MSG_LINE_LEN = 42u;

    // *** Wizard state machine ***********************************************
    enum WizState
    {
      WIZ_NO_CTRL,     // pendant not in control (MPG mode off)
      WIZ_NO_FILE,     // in control, no file list yet / none selected
      WIZ_READY,       // file selected, machine idle
      WIZ_RUNNING,     // job streaming from controller SD
      WIZ_PAUSED,      // M0 hold - operator step in progress
      WIZ_DONE,        // job finished (M30 reached)
      WIZ_ALARM        // grblHAL alarm state
    };

    // *** Private data *******************************************************
    WizState wiz_state = WIZ_NO_CTRL;
    WizState prev_wiz_state = WIZ_ALARM; // force first redraw

    // Job file list (parsed from "[FILE:" lines)
    char file_names[MAX_FILES][MAX_FNAME];
    uint32_t file_cnt = 0u;
    int32_t  file_idx = -1;
    bool list_requested = false;

    // Step prompt lines (parsed from "[MSG:" lines)
    char msg_line1_buf[MSG_LINE_LEN];
    char msg_line2_buf[MSG_LINE_LEN];
    volatile bool msg_updated = false;

    // Set true between job start and IDLE-after-run, used to detect DONE
    bool job_active = false;
    // Stop button needs a second press to confirm
    uint8_t stop_armed = 0u;

    // String buffers for on-screen text
    char state_str_buf[24];
    char file_str_buf[MAX_FNAME + 4];
    char dro_x_buf[20];
    char dro_a_buf[24];
    char btn_main_buf[16];

    // *** Visual objects (DevCore) *******************************************
    String state_str;   // big machine/wizard state banner
    String msg_str1;    // step prompt line 1
    String msg_str2;    // step prompt line 2
    String file_str;    // selected job name
    String dro_x_str;   // X position
    String dro_a_str;   // A position shown as turns
    Box    msg_box;     // frame around the prompt area

    UiButton btn_prev;  // previous file
    UiButton btn_next;  // next file
    UiButton btn_main;  // big context action button
    UiButton btn_stop;  // stop/reset (press twice)

    // Screen area given by the framework
    int32_t scr_y = 0;
    int32_t scr_h = 0;

    // *** Private functions **************************************************
    void UpdateStateMachine();
    void UpdateLabels();
    void RequestFileList();
    void StartSelectedJob();
    void HandleMainButton();
    static void GrblLineSink(const char* line); // hooked into GrblComm RX

    // Singleton: private constructor and no copies
    WrapJobScr() {};
    WrapJobScr(WrapJobScr const&) = delete;
    void operator=(WrapJobScr const&) = delete;
};

#endif // WrapJobScr_h
