/*
 * ldisc.c: PuTTY line discipline. Sits between the input coming
 * from keypresses in the window, and the output channel leading to
 * the back end. Implements echo and/or local line editing,
 * depending on what's currently configured.
 */

#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>

#include "putty.h"
#include "terminal.h"

typedef enum InputType { NORMAL, DEDICATED, NONINTERACTIVE } InputType;

struct input_chunk {
    struct input_chunk *next;
    InputType type;
    size_t size;
};

typedef enum LdiscSendEventType {
    LDISC_SEND_DATA,
    LDISC_SEND_SPECIAL,
} LdiscSendEventType;

struct ldisc_send_event {
    struct ldisc_send_event *next;
    LdiscSendEventType type;
    size_t size;
    SessionSpecialCode code;
    int arg;
};

struct Ldisc_tag {
    Terminal *term;
    Backend *backend;
    Seat *seat;

    /*
     * When the backend is not reporting true from sendok(), we must
     * buffer the input received by ldisc_send(). It's stored in the
     * bufchain below, together with a linked list of input_chunk
     * blocks storing the extra metadata about special keys and
     * interactivity that ldisc_send() receives.
     *
     * All input is added to this buffer initially, but we then
     * process as much of it as possible immediately and hand it off
     * to the backend or a TermLineEditor. Anything left stays in this
     * buffer until ldisc_check_sendok() is next called, triggering a
     * run of the callback that tries again to process the queue.
     */
    bufchain input_queue;
    struct input_chunk *inchunk_head, *inchunk_tail;

    bufchain send_queue;
    struct ldisc_send_event *send_head, *send_tail;
    int send_rate_limit;
    unsigned long send_next_tick;
    bool send_timer_active;

    IdempotentCallback input_queue_callback;

    /*
     * Values cached out of conf.
     */
    bool telnet_keyboard, telnet_newline;
    int protocol, localecho, localedit;

    TermLineEditor *le;
    TermLineEditorCallbackReceiver le_rcv;

    /* We get one of these communicated to us by
     * term_get_userpass_input while it's reading a prompt, so that we
     * can push data straight into it */
    TermLineEditor *userpass_le;
};

#define ECHOING (ldisc->localecho == FORCE_ON || \
                 (ldisc->localecho == AUTO && \
                      (backend_ldisc_option_state(ldisc->backend, LD_ECHO))))
#define EDITING (ldisc->localedit == FORCE_ON || \
                 (ldisc->localedit == AUTO && \
                      (backend_ldisc_option_state(ldisc->backend, LD_EDIT))))

static void ldisc_input_queue_callback(void *ctx);
static unsigned ldisc_send_interval_ticks(Ldisc *ldisc);
static void ldisc_send_queue_try(Ldisc *ldisc);
static void ldisc_send_queue_timer(void *ctx, unsigned long now);
static void ldisc_queue_data(Ldisc *ldisc, const void *data, size_t len);
static void ldisc_queue_special(Ldisc *ldisc, SessionSpecialCode code, int arg);

static const TermLineEditorCallbackReceiverVtable ldisc_lineedit_receiver_vt;

#define CTRL(x) (x^'@')

Ldisc *ldisc_create(Conf *conf, Terminal *term, Backend *backend, Seat *seat)
{
    Ldisc *ldisc = snew(Ldisc);
    memset(ldisc, 0, sizeof(Ldisc));

    ldisc->backend = backend;
    ldisc->term = term;
    ldisc->seat = seat;

    bufchain_init(&ldisc->input_queue);
    bufchain_init(&ldisc->send_queue);

    ldisc->input_queue_callback.fn = ldisc_input_queue_callback;
    ldisc->input_queue_callback.ctx = ldisc;
    bufchain_set_callback(&ldisc->input_queue, &ldisc->input_queue_callback);

    if (ldisc->term) {
        ldisc->le_rcv.vt = &ldisc_lineedit_receiver_vt;
        ldisc->le = lineedit_new(ldisc->term, 0, &ldisc->le_rcv);
    }

    ldisc_configure(ldisc, conf);

    /* Link ourselves into the backend and the terminal */
    if (term)
        term->ldisc = ldisc;
    if (backend)
        backend_provide_ldisc(backend, ldisc);

    return ldisc;
}

void ldisc_configure(Ldisc *ldisc, Conf *conf)
{
    int old_send_rate_limit = ldisc->send_rate_limit;
    int new_send_rate_limit = conf_get_int(conf, CONF_send_rate_limit);

    ldisc->telnet_keyboard = conf_get_bool(conf, CONF_telnet_keyboard);
    ldisc->telnet_newline = conf_get_bool(conf, CONF_telnet_newline);
    ldisc->protocol = conf_get_int(conf, CONF_protocol);
    ldisc->localecho = conf_get_int(conf, CONF_localecho);
    ldisc->localedit = conf_get_int(conf, CONF_localedit);
    if (new_send_rate_limit < 0)
        new_send_rate_limit = 0;
    if (old_send_rate_limit != new_send_rate_limit) {
        ldisc->send_rate_limit = new_send_rate_limit;
        expire_timer_context(ldisc);
        ldisc->send_timer_active = false;
        if (new_send_rate_limit > 0)
            ldisc->send_next_tick =
                GETTICKCOUNT() + ldisc_send_interval_ticks(ldisc);
        else
            ldisc->send_next_tick = 0;
        ldisc_send_queue_try(ldisc);
    }

    unsigned flags = 0;
    if (ldisc->protocol == PROT_RAW)
        flags |= LE_CRLF_NEWLINE;
    if (ldisc->telnet_keyboard)
        flags |= LE_INTERRUPT | LE_SUSPEND | LE_ABORT;
    lineedit_modify_flags(ldisc->le, ~0U, flags);
}

void ldisc_free(Ldisc *ldisc)
{
    bufchain_clear(&ldisc->input_queue);
    bufchain_clear(&ldisc->send_queue);
    while (ldisc->inchunk_head) {
        struct input_chunk *oldhead = ldisc->inchunk_head;
        ldisc->inchunk_head = ldisc->inchunk_head->next;
        sfree(oldhead);
    }
    while (ldisc->send_head) {
        struct ldisc_send_event *oldhead = ldisc->send_head;
        ldisc->send_head = ldisc->send_head->next;
        sfree(oldhead);
    }
    lineedit_free(ldisc->le);
    if (ldisc->term)
        ldisc->term->ldisc = NULL;
    if (ldisc->backend)
        backend_provide_ldisc(ldisc->backend, NULL);
    expire_timer_context(ldisc);
    delete_callbacks_for_context(ldisc);
    sfree(ldisc);
}

void ldisc_echoedit_update(Ldisc *ldisc)
{
    seat_echoedit_update(ldisc->seat, ECHOING, EDITING);

    /*
     * If we've just turned off local line editing mode, and our
     * TermLineEditor had a partial buffer, then send the contents of
     * the buffer. Rationale: (a) otherwise you lose data; (b) the
     * user quite likely typed the buffer contents _anticipating_ that
     * local editing would be turned off shortly, and the event was
     * slow arriving.
     */
    if (!EDITING)
        lineedit_send_line(ldisc->le);
}

void ldisc_provide_userpass_le(Ldisc *ldisc, TermLineEditor *le)
{
    /*
     * Called by term_get_userpass_input to tell us when it has its
     * own TermLineEditor processing a password prompt, so that we can
     * inject our input into that instead of putting it into our own
     * TermLineEditor or sending it straight to the backend.
     */
    ldisc->userpass_le = le;
}

static inline bool is_dedicated_byte(char c, InputType type)
{
    switch (type) {
      case DEDICATED:
        return true;
      case NORMAL:
        return false;
      case NONINTERACTIVE:
        /*
         * Non-interactive input (e.g. from a paste) doesn't come with
         * the ability to distinguish dedicated keypresses like Return
         * from generic ones like Ctrl+M. So we just have to make up
         * an answer to this question. In particular, we _must_ treat
         * Ctrl+M as the Return key, because that's the only way a
         * newline can be pasted at all.
         */
        return c == '\r';
      default:
        unreachable("those values should be exhaustive");
    }
}

static void ldisc_input_queue_consume(Ldisc *ldisc, size_t size)
{
    bufchain_consume(&ldisc->input_queue, size);
    while (size > 0) {
        size_t thissize = (size < ldisc->inchunk_head->size ?
                           size : ldisc->inchunk_head->size);
        ldisc->inchunk_head->size -= thissize;
        size -= thissize;

        if (!ldisc->inchunk_head->size) {
            struct input_chunk *oldhead = ldisc->inchunk_head;
            ldisc->inchunk_head = ldisc->inchunk_head->next;
            if (!ldisc->inchunk_head)
                ldisc->inchunk_tail = NULL;
            sfree(oldhead);
        }
    }
}

static unsigned ldisc_send_interval_ticks(Ldisc *ldisc)
{
    assert(ldisc->send_rate_limit > 0);
    return 1 + (TICKSPERSEC - 1) / (unsigned)ldisc->send_rate_limit;
}

static bool ldisc_send_tick_due(unsigned long now, unsigned long due)
{
    return now - due < INT_MAX;
}

static void ldisc_send_event_append(Ldisc *ldisc, struct ldisc_send_event *ev)
{
    ev->next = NULL;
    if (ldisc->send_tail)
        ldisc->send_tail->next = ev;
    else
        ldisc->send_head = ev;
    ldisc->send_tail = ev;
}

static void ldisc_send_event_pop(Ldisc *ldisc)
{
    struct ldisc_send_event *oldhead = ldisc->send_head;
    ldisc->send_head = oldhead->next;
    if (!ldisc->send_head)
        ldisc->send_tail = NULL;
    sfree(oldhead);
}

static void ldisc_send_queue_arm(Ldisc *ldisc, unsigned long now)
{
    unsigned long wait;

    if (ldisc->send_timer_active)
        return;

    wait = ldisc_send_tick_due(now, ldisc->send_next_tick) ?
        1 : ldisc->send_next_tick - now;
    ldisc->send_timer_active = true;
    schedule_timer(wait, ldisc_send_queue_timer, ldisc);
}

static void ldisc_send_queue_try(Ldisc *ldisc)
{
    while (ldisc->send_head && backend_sendok(ldisc->backend)) {
        struct ldisc_send_event *ev = ldisc->send_head;

        if (ev->type == LDISC_SEND_DATA) {
            ptrlen pl = bufchain_prefix(&ldisc->send_queue);
            size_t len;

            assert(pl.len > 0);
            if (ldisc->send_rate_limit > 0) {
                unsigned long now = GETTICKCOUNT();
                if (!ldisc_send_tick_due(now, ldisc->send_next_tick)) {
                    ldisc_send_queue_arm(ldisc, now);
                    return;
                }
                len = 1;
            } else {
                len = ev->size;
            }

            if (len > pl.len)
                len = pl.len;
            backend_send(ldisc->backend, pl.ptr, len);
            bufchain_consume(&ldisc->send_queue, len);
            ev->size -= len;
            if (ldisc->send_rate_limit > 0)
                ldisc->send_next_tick =
                    GETTICKCOUNT() + ldisc_send_interval_ticks(ldisc);
            if (ev->size) {
                if (ldisc->send_rate_limit > 0)
                    ldisc_send_queue_arm(ldisc, GETTICKCOUNT());
                return;
            }
            ldisc_send_event_pop(ldisc);
        } else {
            backend_special(ldisc->backend, ev->code, ev->arg);
            ldisc_send_event_pop(ldisc);
        }
    }
}

static void ldisc_send_queue_timer(void *ctx, unsigned long now)
{
    Ldisc *ldisc = (Ldisc *)ctx;
    ldisc->send_timer_active = false;
    ldisc_send_queue_try(ldisc);
}

static void ldisc_queue_data(Ldisc *ldisc, const void *data, size_t len)
{
    struct ldisc_send_event *ev;

    if (!len)
        return;

    ev = snew(struct ldisc_send_event);
    ev->type = LDISC_SEND_DATA;
    ev->size = len;
    ev->code = SS_NOP;
    ev->arg = 0;
    bufchain_add(&ldisc->send_queue, data, len);
    ldisc_send_event_append(ldisc, ev);
    ldisc_send_queue_try(ldisc);
}

static void ldisc_queue_special(Ldisc *ldisc, SessionSpecialCode code, int arg)
{
    struct ldisc_send_event *ev = snew(struct ldisc_send_event);

    ev->type = LDISC_SEND_SPECIAL;
    ev->size = 0;
    ev->code = code;
    ev->arg = arg;
    ldisc_send_event_append(ldisc, ev);
    ldisc_send_queue_try(ldisc);
}

static void ldisc_lineedit_to_terminal(
    TermLineEditorCallbackReceiver *rcv, ptrlen data)
{
    Ldisc *ldisc = container_of(rcv, Ldisc, le_rcv);
    if (ECHOING)
        seat_stdout(ldisc->seat, data.ptr, data.len);
}

static void ldisc_lineedit_to_backend(
    TermLineEditorCallbackReceiver *rcv, ptrlen data)
{
    Ldisc *ldisc = container_of(rcv, Ldisc, le_rcv);
    ldisc_queue_data(ldisc, data.ptr, data.len);
}

static void ldisc_lineedit_special(
    TermLineEditorCallbackReceiver *rcv, SessionSpecialCode code, int arg)
{
    Ldisc *ldisc = container_of(rcv, Ldisc, le_rcv);
    ldisc_queue_special(ldisc, code, arg);
}

static void ldisc_lineedit_newline(TermLineEditorCallbackReceiver *rcv)
{
    Ldisc *ldisc = container_of(rcv, Ldisc, le_rcv);
    if (ldisc->protocol == PROT_RAW)
        ldisc_queue_data(ldisc, "\r\n", 2);
    else if (ldisc->protocol == PROT_TELNET && ldisc->telnet_newline)
        ldisc_queue_special(ldisc, SS_EOL, 0);
    else
        ldisc_queue_data(ldisc, "\r", 1);
}

static const TermLineEditorCallbackReceiverVtable
ldisc_lineedit_receiver_vt = {
    .to_terminal = ldisc_lineedit_to_terminal,
    .to_backend = ldisc_lineedit_to_backend,
    .special = ldisc_lineedit_special,
    .newline = ldisc_lineedit_newline,
};

void ldisc_check_sendok(Ldisc *ldisc)
{
    ldisc_send_queue_try(ldisc);
    queue_idempotent_callback(&ldisc->input_queue_callback);
}

void ldisc_send(Ldisc *ldisc, const void *vbuf, int len, bool interactive)
{
    assert(ldisc->term);

    if (interactive) {
        /*
         * Interrupt a paste from the clipboard, if one was in
         * progress when the user pressed a key. This is easier than
         * buffering the current piece of data and saving it until the
         * terminal has finished pasting, and has the potential side
         * benefit of permitting a user to cancel an accidental huge
         * paste.
         */
        term_nopaste(ldisc->term);
    }

    InputType type;
    if (len < 0) {
        /*
         * Less than zero means null terminated special string.
         */
        len = strlen(vbuf);
        type = DEDICATED;
    } else if (len > 0) {
        type = interactive ? NORMAL : NONINTERACTIVE;
    } else {
        return; /* nothing to do anyway */
    }

    /*
     * Append our data to input_queue, and ensure it's marked with the
     * right type.
     */
    bufchain_add(&ldisc->input_queue, vbuf, len);
    if (!(ldisc->inchunk_tail && ldisc->inchunk_tail->type == type)) {
        struct input_chunk *new_chunk = snew(struct input_chunk);

        new_chunk->type = type;
        new_chunk->size = 0;

        new_chunk->next = NULL;
        if (ldisc->inchunk_tail)
            ldisc->inchunk_tail->next = new_chunk;
        else
            ldisc->inchunk_head = new_chunk;
        ldisc->inchunk_tail = new_chunk;
    }
    ldisc->inchunk_tail->size += len;

    /*
     * And process as much of the data immediately as we can.
     */
    ldisc_input_queue_callback(ldisc);
}

static void ldisc_input_queue_callback(void *ctx)
{
    Ldisc *ldisc = (Ldisc *)ctx;

    /*
     * Toplevel callback that is triggered whenever the input queue
     * lengthens.
     */
    while (bufchain_size(&ldisc->input_queue)) {
        ptrlen pl = bufchain_prefix(&ldisc->input_queue);
        const char *start = pl.ptr, *buf = pl.ptr;
        size_t len = (pl.len < ldisc->inchunk_head->size ?
                      pl.len : ldisc->inchunk_head->size);
        InputType type = ldisc->inchunk_head->type;

        while (len > 0 && ldisc->userpass_le) {
            char c = *buf++;
            len--;

            bool dedicated = is_dedicated_byte(c, type);
            lineedit_input(ldisc->userpass_le, c, dedicated);
        }

        if (!backend_sendok(ldisc->backend)) {
            ldisc_input_queue_consume(ldisc, buf - start);
            break;
        }

        /*
         * Either perform local editing, or just send characters.
         */
        if (EDITING) {
            while (len > 0) {
                char c = *buf++;
                len--;

                bool dedicated = is_dedicated_byte(c, type);
                lineedit_input(ldisc->le, c, dedicated);
            }
            ldisc_input_queue_consume(ldisc, buf - start);
        } else {
            if (ECHOING)
                seat_stdout(ldisc->seat, buf, len);
            if (type == DEDICATED && ldisc->protocol == PROT_TELNET) {
                while (len > 0) {
                    char c = *buf++;
                    len--;
                    switch (c) {
                      case CTRL('M'):
                        if (ldisc->telnet_newline)
                            ldisc_queue_special(ldisc, SS_EOL, 0);
                        else
                            ldisc_queue_data(ldisc, "\r", 1);
                        break;
                      case CTRL('?'):
                      case CTRL('H'):
                        if (ldisc->telnet_keyboard) {
                            ldisc_queue_special(ldisc, SS_EC, 0);
                            break;
                        }
                      case CTRL('C'):
                        if (ldisc->telnet_keyboard) {
                            ldisc_queue_special(ldisc, SS_IP, 0);
                            break;
                        }
                      case CTRL('Z'):
                        if (ldisc->telnet_keyboard) {
                            ldisc_queue_special(ldisc, SS_SUSP, 0);
                            break;
                        }

                      default:
                        ldisc_queue_data(ldisc, &c, 1);
                        break;
                    }
                }
                ldisc_input_queue_consume(ldisc, buf - start);
            } else {
                ldisc_queue_data(ldisc, buf, len);
                ldisc_input_queue_consume(ldisc, len);
            }
        }
    }
}
