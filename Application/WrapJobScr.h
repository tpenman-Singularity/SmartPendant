//******************************************************************************
//  @file WrapJobScr.h
//  @brief Wire Wrap guided-job screen for SmartPendant (grblHAL) - header
//
//  Hybrid design: the pendant streams the .nc job file line-by-line from the
//  SD card (exactly like ProgramSender does), and because IT is the one
//  sending each line, it can show the step prompt the moment it streams a
//  line that starts with "(MSG,". Lines with M0 make the controller pause;
//  the operator taps the big CONTINUE button (which just sends Run/~).
//
//  This needs ZERO changes to GrblComm. It reuses the same streaming pattern,
//  SD access (fatfs), and soft-button sharing that ProgramSender uses.
//
//  Structure mirrors ProgramSender.h so it matches the real IScreen API.
//******************************************************************************

#ifndef WrapJobScr_h
#define WrapJobScr_h

// *****************************************************************************
// ***   Includes   ************************************************************
// *****************************************************************************
#include "DevCore.h"

#include "IScreen.h"
#include "DataWindow.h"
#include "GrblComm.h"
#include "InputDrv.h"
#include "Menu.h"
#include "TextBox.h"
#include "fatfs.h"   // FatFS types (FIL) used by the streamed job file

// *****************************************************************************
// ***   WrapJobScr Class   ****************************************************
// *****************************************************************************
class WrapJobScr : public IScreen
{
  public:
    // *** Get Instance ********************************************************
    static WrapJobScr& GetInstance();

    // *** IScreen interface (same signatures as ProgramSender) ***************
    virtual Result Setup(int32_t y, int32_t height);
    virtual Result Show();
    virtual Result Hide();
    virtual Result TimerExpired(uint32_t interval);
    virtual Result ProcessCallback(const void* ptr);

  private:
    static const uint8_t  BORDER_W = 4u;
    // Max job files listed from SD
    static const uint32_t MAX_FILES = 32u;
    // Line buffer (matches ProgramSender's 80+CRLF+slack convention)
    static const uint32_t LINE_BUF = 128u;
    // On-screen step-prompt buffer
    static const uint32_t MSG_BUF = 96u;

    // *** Wizard / streaming state *******************************************
    bool run = false;        // currently streaming
    bool finished = false;   // reached end of file
    bool paused_at_m0 = false; // last streamed line was M0 -> waiting operator
    uint32_t id = 0u;        // current command id (for GetCmdResult)

    // Currently open job file (streamed line-by-line, never fully buffered,
    // so long wrap files don't need a big RAM allocation)
    FIL job_file;
    bool file_open = false;

    // Current step-prompt text shown big on screen
    char msg_buf[MSG_BUF] = {0};

    // *** SD file menu (same pattern as ProgramSender) ***********************
    char str[MAX_FILES][32u + 1u] = {0};
    Menu::MenuItem menu_items[MAX_FILES];
    Menu menu;

    // *** Visual objects *****************************************************
    // Big step-prompt lines
    String  msg_title;   // e.g. "STEP 3 OF 6"
    String  msg_line1;   // prompt text line 1
    String  msg_line2;   // prompt text line 2
    Box     msg_box;     // frame around the prompt

    // Shared soft buttons (owned by Application, like ProgramSender)
    UiButton& left_btn;    // CONTINUE / RUN
    UiButton& middle_btn;  // OPEN (pick job file)
    UiButton& right_btn;   // STOP

    // Axis DRO reuses Application's shared data windows in Show()

    // Instances
    DisplayDrv& display_drv = DisplayDrv::GetInstance();
    GrblComm&   grbl_comm   = GrblComm::GetInstance();

    // Encoder callback entry (for scrolling file menu)
    InputDrv::CallbackListEntry enc_cble;
    int32_t enc_val = 0;

    // *** Private helpers ****************************************************
    void   ShowPrompt(const char* title, const char* l1, const char* l2);
    void   StreamNextLine();
    void   SetPromptFromMsgLine(const char* gcode_line); // parse "(MSG,...)"
    void   OpenFileList();
    void   CloseJob();

    // Menu callbacks (same static-callback pattern as ProgramSender)
    static Result ProcessMenuOkCallback(WrapJobScr* obj_ptr, void* ptr);
    static Result ProcessMenuCancelCallback(WrapJobScr* obj_ptr, void* ptr);
    static Result ProcessEncoderCallback(WrapJobScr* obj_ptr, void* ptr);

    // Singleton: constructor grabs the shared soft buttons like ProgramSender
    WrapJobScr();
    WrapJobScr(WrapJobScr const&) = delete;
    void operator=(WrapJobScr const&) = delete;
};

#endif // WrapJobScr_h
