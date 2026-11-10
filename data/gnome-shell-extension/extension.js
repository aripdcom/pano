// Pano Terminal's quick terminal on GNOME Shell.
//
// On Wayland, applications cannot place their windows or keep them above
// others. The quick terminal has an application ID of its own (see
// terminal_window_set_quick()), so this extension finds it, puts it at the
// top of the monitor with the pointer, centered, keeps it above other
// windows and on all workspaces, and slides it in if Pano Terminal's
// quick-terminal-slide setting is on.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const QUICK_TERMINAL_ID = 'com.aripd.Pano.QuickTerminal';
const SETTINGS_SCHEMA = 'com.aripd.Pano.Legacy.Settings';
const SLIDE_DURATION = 200;

export default class PanoQuickTerminalExtension extends Extension {
    enable() {
        this._settings = this._lookupSettings();
        this._windows = new Map();
        this._createdId = global.display.connect('window-created',
            (display, window) => this._watch(window));
        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);
    }

    disable() {
        global.display.disconnect(this._createdId);
        for (const window of [...this._windows.keys()]) {
            window.get_compositor_private()?.remove_all_transitions();
            this._forget(window);
        }
        this._windows = null;
        this._settings = null;
    }

    // Pano Terminal's settings, if its schema is installed
    _lookupSettings() {
        const source = Gio.SettingsSchemaSource.get_default();
        const schema = source?.lookup(SETTINGS_SCHEMA, true);
        return schema ? new Gio.Settings({settings_schema: schema}) : null;
    }

    _isQuickTerminal(window) {
        return window.get_wm_class() === QUICK_TERMINAL_ID ||
            window.get_gtk_application_id?.() === QUICK_TERMINAL_ID;
    }

    // The application ID may come after the window
    _watch(window) {
        if (this._windows.has(window))
            return;

        const state = {ids: [], placed: false};
        this._windows.set(window, state);
        state.ids.push(window.connect('notify::wm-class', () => this._check(window)));
        state.ids.push(window.connect('unmanaged', () => this._forget(window)));
        this._check(window);
    }

    _forget(window) {
        const state = this._windows?.get(window);
        if (!state)
            return;
        for (const id of state.ids)
            window.disconnect(id);
        this._windows.delete(window);
    }

    _check(window) {
        const state = this._windows.get(window);
        if (!state || state.placed || !this._isQuickTerminal(window))
            return;

        state.placed = true;
        // Its size is known once it is shown
        if (window.get_frame_rect().width > 0 && window.get_compositor_private()?.visible)
            this._place(window);
        else
            state.ids.push(window.connect('shown', () => this._place(window)));
    }

    _place(window) {
        window.make_above();
        window.stick();

        const monitor = global.display.get_current_monitor();
        const area = window.get_work_area_for_monitor(monitor);
        const rect = window.get_frame_rect();
        const x = area.x + Math.max(0, Math.round((area.width - rect.width) / 2));
        window.move_frame(true, x, area.y);
        console.log(`Pano Terminal: placed the quick terminal at ${x},${area.y}`);

        const slide = this._settings?.get_boolean('quick-terminal-slide') ?? true;
        const actor = window.get_compositor_private();
        if (!slide || !actor)
            return;

        actor.remove_all_transitions();
        actor.translation_y = -rect.height;
        actor.ease({
            translation_y: 0,
            duration: SLIDE_DURATION,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
    }
}
