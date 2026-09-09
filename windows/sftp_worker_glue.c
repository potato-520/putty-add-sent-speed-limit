/*
 * sftp_worker_glue.c - Console and helper routines for embedded SFTP worker in putty_webview
 */

#include <stdio.h>
#include <stdlib.h>
#include "putty.h"
#include "ssh.h"
#include "console.h"

const char console_abandoned_msg[] = "Connection abandoned.\n";

bool console_batch_mode = false;

bool console_set_batch_mode(bool newvalue)
{
    console_batch_mode = newvalue;
    return true;
}

bool console_set_stdio_prompts(bool newvalue)
{
    return true;
}

void console_print_error_msg(const char *prefix, const char *msg)
{
    fputs(prefix, stderr);
    fputs(": ", stderr);
    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

void console_connection_fatal(Seat *seat, const char *msg)
{
    console_print_error_msg("FATAL ERROR", msg);
    cleanup_exit(1);
}

void console_nonfatal(Seat *seat, const char *msg)
{
    console_print_error_msg("ERROR", msg);
}

StripCtrlChars *console_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    return stripctrl_new(bs_out, false, L'\0');
}

static void console_eventlog(LogPolicy *lp, const char *event) {}
static int console_askappend(LogPolicy *lp, Filename *fn,
                             void (*callback)(void *ctx, int result), void *ctx) { return 2; }
static void console_logging_error(LogPolicy *lp, const char *event) {}
static const LogPolicyVtable console_cli_logpolicy_vt = {
    .eventlog = console_eventlog,
    .askappend = console_askappend,
    .logging_error = console_logging_error,
    .verbose = cmdline_lp_verbose,
};
LogPolicy console_cli_logpolicy[1] = {{ &console_cli_logpolicy_vt }};

SeatPromptResult console_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    return SPR_SW_ABORT("Cannot confirm a host key in batch mode");
}

SeatPromptResult console_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    return SPR_SW_ABORT("Cannot confirm weak crypto primitive in batch mode");
}

SeatPromptResult console_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    return SPR_SW_ABORT("Cannot confirm weak cached hostkey in batch mode");
}

const SeatDialogPromptDescriptions *console_prompt_descriptions(Seat *seat)
{
    static const SeatDialogPromptDescriptions descs = {
        .hk_accept_action = "enter \"y\"",
        .hk_connect_once_action = "enter \"n\"",
        .hk_cancel_action = "press Return",
        .hk_cancel_action_Participle = "Pressing Return",
        .weak_accept_action = "enter \"y\"",
        .weak_cancel_action = "enter \"n\"",
    };
    return &descs;
}

SeatPromptResult console_get_userpass_input(prompts_t *p)
{
    return SPR_SW_ABORT("Cannot prompt for user/password in batch mode");
}

void pgp_fingerprints(void)
{
    fputs("These are the fingerprints of the PuTTY PGP Master Keys. They can\n"
          "be used to establish a trust path from this executable to another\n"
          "one. See the manual for more information.\n"
          "(Note: these fingerprints have nothing to do with SSH!)\n"
          "\n"
          "PuTTY Master Key as of " PGP_MASTER_KEY_YEAR
          " (" PGP_MASTER_KEY_DETAILS "):\n"
          "  " PGP_MASTER_KEY_FP "\n\n"
          "Previous Master Key (" PGP_PREV_MASTER_KEY_YEAR
          ", " PGP_PREV_MASTER_KEY_DETAILS "):\n"
          "  " PGP_PREV_MASTER_KEY_FP "\n", stdout);
}
