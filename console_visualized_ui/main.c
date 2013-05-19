#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>
#include <unistd.h>
#include <curses.h>
#include <dialog.h>
#include "libdevcheck.h"
#include "device.h"
#include "utils.h"
#include "procedure.h"
#include "vis.h"
#include "ncurses_convenience.h"
#include "dialog_convenience.h"
#include "render.h"

#define ACT_EXIT 0
#define ACT_ATTRS 1
#define ACT_READ 2
#define ACT_ZEROFILL 3

static int global_init(void);
static void global_fini(void);
static int menu_choose_device(DC_DevList *devlist);
static int menu_choose_procedure(DC_Dev *dev);
static void show_smart_attrs(DC_Dev *dev);
void log_cb(enum DC_LogLevel level, const char* fmt, va_list vl);

int main() {
    int r;
    r = global_init();
    if (r) {
        fprintf(stderr, "init fail\n");
        return r;
    }
    // get list of devices
    DC_DevList *devlist = dc_dev_list();
    assert(devlist);

    while (1) {
        // draw menu of device choice
        int chosen_dev_ind;
        chosen_dev_ind = menu_choose_device(devlist);
        if (chosen_dev_ind < 0)
            break;

        DC_Dev *chosen_dev = dc_dev_list_get_entry(devlist, chosen_dev_ind);
        if (!chosen_dev) {
            printw("No device with index %d\n", chosen_dev_ind);
            return 1;
        }
        // draw procedures menu
        int chosen_procedure_ind;
        chosen_procedure_ind = menu_choose_procedure(chosen_dev);
        if (chosen_procedure_ind < 0)
            break;

        switch (chosen_procedure_ind) {
        case ACT_EXIT:
            return 0;
        case ACT_ATTRS:
            show_smart_attrs(chosen_dev);
            break;
        case ACT_READ:
        case ACT_ZEROFILL:
        {
            char *act_name = chosen_procedure_ind == ACT_READ ? "posix_read" : "posix_write_zeros";
            DC_Procedure *act = dc_find_procedure(act_name);
            assert(act);
            if (act->flags & DC_PROC_FLAG_DESTRUCTIVE) {
                char *ask;
                r = asprintf(&ask, "This will destroy all data on device %s (%s). Are you sure?",
                        chosen_dev->dev_fs_name, chosen_dev->model_str);
                assert(r != -1);
                r = dialog_yesno("Confirmation", ask, 0, 0);
                // Yes = 0 (FALSE), No = 1, Escape = -1
                free(ask);
                if (/* No */ r)
                    break;
            }
            DC_ProcedureCtx *actctx;
            r = dc_procedure_open(act, chosen_dev, &actctx);
            if (r) {
                dialog_msgbox("Error", "Procedure init fail", 0, 0, 1);
                continue;
            }
            render_procedure(actctx);
            break;
        }
        default:
            dialog_msgbox("Error", "Wrong procedure index", 0, 0, 1);
            continue;
        }
    } // while(1)

    return 0;
}

static int global_init(void) {
    int r;
    // TODO check all retcodes
    setlocale(LC_ALL, "");
    initscr();
    init_dialog(stdin, stdout);
    dialog_vars.item_help = 0;

    start_color();
    init_my_colors();
    noecho();
    cbreak();
    scrollok(stdscr, FALSE);
    keypad(stdscr, TRUE);

    WINDOW *footer = subwin(stdscr, 1, COLS, LINES-1, 0);
    wbkgd(footer, COLOR_PAIR(MY_COLOR_WHITE_ON_BLUE));
    wprintw(footer, " WHDD rev. " WHDD_VERSION);

    wrefresh(footer);
    // init libdevcheck
    r = dc_init();
    assert(!r);
    dc_log_set_callback(log_cb);
    r = atexit(global_fini);
    assert(r == 0);
    return 0;
}

static void global_fini(void) {
    clear();
    endwin();
}

static int menu_choose_device(DC_DevList *devlist) {
    int r;
    int devs_num = dc_dev_list_size(devlist);
    if (devs_num == 0) {
        dialog_msgbox("Info", "No devices found", 0, 0, 1);
        return -1;
    }

    char **items = calloc( 2 * devs_num, sizeof(char*));
    assert(items);

    int i;
    for (i = 0; i < devs_num; i++) {
        DC_Dev *dev = dc_dev_list_get_entry(devlist, i);
        char *dev_descr;
        r = asprintf(&dev_descr,
                "%s" // model name
                // TODO human-readable size
                " %"PRIu64" bytes" // size
                ,dev->model_str
                ,dev->capacity
              );
        assert(r != -1);
        items[2*i] = strdup(dev->dev_fs_name);
        items[2*i+1] = dev_descr;
    }

    clear_body();
    int chosen_dev_ind = my_dialog_menu("Choose device", "", 0, 0, devs_num * 3, devs_num, items);
    for (i = 0; i < devs_num; i++) {
        free(items[2*i]);
        free(items[2*i+1]);
    }
    free(items);

    return chosen_dev_ind;
}

static int menu_choose_procedure(DC_Dev *dev) {
    char *items[4 * 2];
    items[1] = strdup("Exit");
    items[3] = strdup("Show SMART attributes");
    items[5] = strdup("Perform read test");
    items[7] = strdup("Perform 'write zeros' test");
    int i;
    // this fuckin libdialog makes me code crappy
    for (i = 0; i < 4; i++)
        items[2*i] = strdup("");

    clear_body();
    int chosen_procedure_ind = my_dialog_menu("Choose procedure", "", 0, 0, 4 * 3, 4, items);
    for (i = 0; i < 8; i++)
        free(items[i]);
    return chosen_procedure_ind;
}

static void show_smart_attrs(DC_Dev *dev) {
    char *text;
    text = dc_dev_smartctl_text(dev->dev_path, " -i -s on -A ");
    dialog_msgbox("SMART Attributes", text ? : "Getting attrs failed", LINES-6, COLS, 1);
    if (text)
        free(text);
    refresh();
}

void log_cb(enum DC_LogLevel level, const char* fmt, va_list vl) {
    char *msg = dc_log_default_form_string(level, fmt, vl);
    assert(msg);
    if (render_ctx_global) {
        wprintw(render_ctx_global->w_log, "%s", msg);
        wrefresh(render_ctx_global->w_log);
    } else {
        dialog_msgbox("Internal message", msg, 0, 0, 1);
    }
    free(msg);
}
