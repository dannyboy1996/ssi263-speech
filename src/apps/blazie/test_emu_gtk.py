"""The GTK emulator (blazie_emu_gtk), as Orca meets it: the program run in a virtual X display (Xvfb), its keys typed
through the X server (xdotool: XTest, so GDK's own key events, down and up), its window read through the
accessibility bus (AT-SPI, gi's Atspi: what Orca reads).  No sound card (--no-sound: the system clock paces the
unit); --trace tells the test the unit's level, its keys and its status lines.

    dbus-run-session -- xvfb-run -a -s "-screen 0 1024x768x24" \
        python3 src/apps/blazie/test_emu_gtk.py BLAZIE_EMU_GTK FIRMWARE_DIR [--only NAME,NAME]

FIRMWARE_DIR as test_emu_linux.py's (BL2ENG.BNS + bl2_2003_warm.state; BL2SPA.BNS; tns/TNSENG.TNS).  The checks:
  settings   the settings file the GTK shell writes the first time is the terminal shell's, word for word
  tree       the menu bar's items by name (Firmware, Settings, Help; the units, Export, Import, Exit, Sample rate,
             Serial port, Keys, About), the keyboard area focused with its name and role, the status bar
  chord      the Braille Lite boots and speaks; F (dot 1) typed is sent as dot 1 and the unit answers
  export     Firmware > Export: GTK's file chooser, a path typed, the image written; the result announced
  menu       F11 and Alt+Shift+F open the Firmware menu; Escape gives the keyboard back
  dialog     Help > About through the screen reader's own action: its text read from the alert, closed with OK
  announce   Firmware > the Spanish Braille Lite: "Switched to ..." announced (object:announcement), the area renamed
  tns        Firmware > Type 'n Speak: its first-start dialog read and closed; y, F10 and Alt+Shift+F as its keys
  exit       Firmware > Exit: the program ends, every unit used saved
  held       (a run of its own) p-chord, l, then i-chord held through the restart: the cold reset's question
Controls (tools/linux_tests.sh judges them by their marks): BLAZIE_GTK_BREAK=noname leaves the keyboard area
unnamed -- tree must FAIL for that alone; BLAZIE_KEYS_BREAK=1 swaps dots 1 and 4 -- held must FAIL.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib   # noqa: E402

failures = 0
ran = 0
HERE = os.path.dirname(os.path.abspath(__file__))


def check(name, ok, detail):
    global failures, ran
    print("%-4s %-36s %s" % ("ok" if ok else "FAIL", name, detail))
    sys.stdout.flush()
    failures += not ok
    ran += 1


def pump(seconds=0.0):
    """The accessibility bus's events delivered, for `seconds`."""
    ctx = GLib.MainContext.default()
    end = time.time() + seconds
    while True:
        while ctx.iteration(False):
            pass
        if time.time() >= end:
            return
        time.sleep(0.02)


def wait(cond, timeout, step=0.1):
    end = time.time() + timeout
    while True:
        pump()
        v = cond()
        if v or time.time() >= end:
            return v
        time.sleep(step)


def xdo(*args):
    return subprocess.run(["xdotool"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True, timeout=30).stdout


# ---- the accessibility tree -------------------------------------------------------------------------------------
def walk(obj, depth=0, limit=12):
    if obj is None or depth > limit:
        return
    yield obj
    try:
        n = obj.get_child_count()
    except GLib.Error:
        return
    for i in range(n):
        try:
            c = obj.get_child_at_index(i)
        except GLib.Error:
            continue
        yield from walk(c, depth + 1, limit)


def role(o):
    try:
        return o.get_role_name()
    except GLib.Error:
        return ""


def name(o):
    try:
        return o.get_name() or ""
    except GLib.Error:
        return ""


def has_state(o, st):
    try:
        return o.get_state_set().contains(st)
    except GLib.Error:
        return False


def find_app(pid):
    desk = Atspi.get_desktop(0)
    for i in range(desk.get_child_count()):
        a = desk.get_child_at_index(i)
        try:
            if a is not None and a.get_process_id() == pid:
                return a
        except GLib.Error:
            continue
    return None


def find(app, want_role, want_name=None, pred=None):
    if app is None:
        return None
    app.clear_cache()
    for o in walk(app):
        if role(o) == want_role and (want_name is None or name(o) == want_name) and (pred is None or pred(o)):
            return o
    return None


def menu_items(menu):
    return [name(menu.get_child_at_index(i)) for i in range(menu.get_child_count())]


def labels(o):
    return [name(x) for x in walk(o) if role(x) == "label"]


def click(o):
    o.do_action(0)


def button(o, label):
    """A dialog's button by its label (AT-SPI 2.56 calls the role "button", older ones "push button")."""
    return find(o, "button", label) or find(o, "push button", label)


# ---- the program and its trace -------------------------------------------------------------------------------------
class Emu:
    def __init__(self, exe, fw, tmp, tag, extra=(), env=None):
        self.trace = os.path.join(tmp, tag + ".trace")
        self.cfg = os.path.join(tmp, tag + "-config")
        cmd = [exe, "--firmware", fw, "--config", self.cfg, "--no-sound", "--rate", "22050", "--trace", self.trace]
        cmd += list(extra)
        e = dict(os.environ)
        e.update(env or {})
        self.log = open(os.path.join(tmp, tag + ".log"), "w")
        self.p = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT, env=e)
        self.app = None

    def lines(self):
        try:
            with open(self.trace) as f:
                return f.read().splitlines()
        except OSError:
            return []

    def levels(self):
        """(unit time, rms) of the current unit's run (from its last start)."""
        out = []
        for ln in self.lines():
            if ln.startswith("starting "):
                out = []
            m = re.match(r"^([0-9.]+) rms ([0-9.]+)$", ln)
            if m:
                out.append((float(m.group(1)), float(m.group(2))))
        return out

    def connect(self, timeout=30):
        self.app = wait(lambda: find_app(self.p.pid), timeout, 0.25)
        return self.app

    def area(self):
        return find(self.app, "panel", pred=lambda o: has_state(o, Atspi.StateType.FOCUSABLE))

    def focus(self):
        """The window has the keyboard (no window manager under Xvfb: X's focus set on it)."""
        a = self.area()
        if a is not None and has_state(a, Atspi.StateType.FOCUSED):
            return True
        for wid in xdo("search", "--onlyvisible", "--pid", str(self.p.pid), "--name", "Blazie emulator").split():
            xdo("windowfocus", wid)
        return wait(lambda: (lambda x: x is not None and has_state(x, Atspi.StateType.FOCUSED))(self.area()), 5)

    def focused(self):
        a = self.area()
        return a is not None and has_state(a, Atspi.StateType.FOCUSED)

    def menu_open(self):
        """The Firmware menu open from the keyboard: its first item selected and showing (its name), else None."""
        fwm = find(self.app, "menu", "Firmware")
        if fwm is None or fwm.get_child_count() == 0:
            return None
        first = fwm.get_child_at_index(0)
        if has_state(first, Atspi.StateType.SELECTED) and has_state(first, Atspi.StateType.SHOWING):
            return name(first)
        return None

    def close_menu(self):
        """Escape until the keyboard area has the focus again (one closes the menu; a second only if needed)."""
        for _ in range(2):
            xdo("key", "Escape")
            if wait(self.focused, 3):
                return True
        return False

    def quiet_after_boot(self, timeout=40):
        """The boot greeting heard, then 0.6 s of quiet: (greeting's peak rms, unit time of the quiet)."""
        def done():
            lv = self.levels()
            peak = max([r for t, r in lv if t < 8.0] or [0.0])
            tail = lv[-6:]
            if peak > 0.01 and len(tail) == 6 and all(r < 0.002 for t, r in tail):
                return peak, tail[-1][0]
            return None
        return wait(done, timeout, 0.2) or (max([r for t, r in self.levels()] or [0.0]), -1.0)

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.log.close()


def default_ini_text(path):
    """The DEFAULT_INI string literal of a C file, joined."""
    src = open(path).read()
    m = re.search(r"static const char DEFAULT_INI\[\] =\s*(.*?);\n", src, re.S)
    if not m:
        return None
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))


# ---- the checks ------------------------------------------------------------------------------------------------------
def run_main(exe, fw, tmp, want):
    emu = Emu(exe, fw, tmp, "main", ["--unit", "bl-en"])
    try:
        app = emu.connect()
        if app is None:
            check("the program on the accessibility bus", False, "not found in 30 s (log: %s)"
                  % open(emu.log.name).read()[-300:].replace("\n", " | "))
            return
        events = []
        listener = Atspi.EventListener.new(lambda e: events.append((e.type, str(e.any_data))))
        listener.register("object:announcement")

        if want("settings"):
            ini = os.path.join(emu.cfg, "blazie_emu.ini")
            written = open(ini).read() if os.path.isfile(ini) else ""
            gtk_text = default_ini_text(os.path.join(HERE, "main_gtk.c"))
            term_text = default_ini_text(os.path.join(HERE, "main_linux.c"))
            check("settings file = the terminal shell's", gtk_text is not None and gtk_text == term_text
                  and written.startswith("; The Blazie emulator's settings (Linux)"),
                  "main_gtk.c's DEFAULT_INI %s main_linux.c's; written first: %s"
                  % ("=" if gtk_text == term_text else "DIFFERS FROM", "yes" if written else "no"))

        focused = emu.focus()
        if want("tree"):
            bar = find(app, "menu bar")
            tops = menu_items(bar) if bar is not None else []
            fwm = find(app, "menu", "Firmware")
            stm = find(app, "menu", "Settings")
            hlp = find(app, "menu", "Help")
            items = (menu_items(fwm) if fwm else []) + (menu_items(stm) if stm else []) + \
                (menu_items(hlp) if hlp else [])
            need = ["Braille Lite 2000, English", "Braille Lite 2000, Spanish", "Type 'n Speak, English",
                    "Type 'n Speak, Spanish", "Export files to disk image (.img)...",
                    "Import files from disk image (.img)...",
                    "Back to the factory state (erases this unit's files)...", "Exit", "Sample rate", "Serial port",
                    "Quick key response (faster than the real unit)", "Keys", "About"]
            missing = [n for n in need if n not in items]
            check("menu bar: items by name", tops == ["Firmware", "Settings", "Help"] and not missing,
                  "menu bar %s; %d items named%s" % (tops, len(items),
                                                     "; MISSING %s" % missing if missing else ""))
            area = emu.area()
            nm = name(area) if area is not None else ""
            check("keyboard area: named, focused", area is not None and focused
                  and nm.startswith("Braille Lite 2000 (English): keyboard") and "F11" in nm,
                  "role %s, %s, name %r" % (role(area) if area is not None else "none",
                                            "focused" if focused else "NOT focused", nm))
            sb = find(app, "status bar")
            check("status bar", sb is not None and "is on" in name(sb), "%r" % (name(sb) if sb else None))

        if want("chord"):
            peak, quiet_at = emu.quiet_after_boot()
            xdo("keydown", "f", "sleep", "0.06", "keyup", "f")
            sent = wait(lambda: [ln for ln in emu.lines() if re.match(r"^[0-9.]+ chord 0x", ln)], 3)
            time.sleep(2.5)
            at = float(sent[0].split()[0]) if sent else -1.0
            lv = emu.levels()
            before = max([r for t, r in lv if at - 0.5 <= t < at] or [1.0])
            after = max([r for t, r in lv if at < t <= at + 2.5] or [0.0])
            check("boot greeting", peak > 0.01 and quiet_at > 0, "peak rms %.4f, quiet at %.1f s" % (peak, quiet_at))
            check("a chord answered (F typed)", bool(sent) and " 0x01 " in sent[0] and before < 0.002
                  and after > 0.01, "sent %r; rms %.4f before, %.4f after" % (sent[0] if sent else None, before,
                                                                               after))

        if want("export"):
            item = find(app, "menu item", "Export files to disk image (.img)...")
            out = os.path.join(tmp, "exported.img")
            n0 = len(events)
            if item is not None:
                click(item)
            chooser = wait(lambda: find(app, "file chooser"), 10)
            if chooser is not None:
                time.sleep(0.5)
                xdo("key", "ctrl+a")
                xdo("type", "--delay", "5", out)
                xdo("key", "Return")
            ann = wait(lambda: [t for k, t in events[n0:] if t.startswith("Exported ")], 15)
            size = os.path.getsize(out) if os.path.isfile(out) else 0
            check("export: file chooser, image, announced", chooser is not None and size > 0 and bool(ann)
                  and out in ann[0], "chooser %s; %s %d bytes; announced %r" % (
                      "shown" if chooser is not None else "NOT shown", out, size, ann[0] if ann else None))
            emu.focus()

        if want("menu"):
            for keys in (("F11",), ("alt+shift+f",)):
                n_menu = sum(1 for ln in emu.lines() if ln == "menu")
                xdo("key", *keys)
                first = wait(emu.menu_open, 5)
                opened = bool(first) and sum(1 for ln in emu.lines() if ln == "menu") == n_menu + 1
                time.sleep(0.5)
                back = emu.close_menu()
                check("%s opens the menu" % keys[0], opened and back, "%s; keyboard area %s" % (
                    "Firmware menu open at %r (selected)" % first if opened else "Firmware menu NOT open",
                    "focused again after Escape" if back else "NOT focused"))

        if want("dialog"):
            item = find(app, "menu item", "About")
            if item is not None:
                click(item)
            alert = wait(lambda: find(app, "alert"), 10)
            title = name(alert) if alert is not None else None
            text = " ".join(labels(alert)) if alert is not None else ""
            ok_btn = button(alert, "OK") if alert is not None else None
            if ok_btn is not None:
                click(ok_btn)
            gone = wait(lambda: find(app, "alert") is None, 5)
            back = wait(emu.focused, 5)
            check("a dialog's text (Help > About)", title == "About" and "MIT" in text and "Blazie" in text
                  and gone and back, "alert %r: %r...; %s; keyboard area %s" % (
                      title, text[:50], "closed with OK" if gone else "NOT closed",
                      "focused again" if back else "NOT focused"))
            emu.focus()

        if want("announce"):
            item = find(app, "radio menu item", "Braille Lite 2000, Spanish")
            n0 = len(events)
            if item is not None:
                click(item)
            ann = wait(lambda: [t for k, t in events[n0:] if t.startswith("Switched to")], 20)
            renamed = wait(lambda: name(emu.area()).startswith("Braille Lite 2000 (Spanish): keyboard"), 5)
            check("announced: the unit switched", bool(ann) and "Spanish" in ann[0] and renamed,
                  "object:announcement %r; area %s" % (ann[0] if ann else None,
                                                      "renamed" if renamed else "NOT renamed: %r" % name(emu.area())))
            emu.focus()

        if want("tns"):
            item = find(app, "radio menu item", "Type 'n Speak, English")
            if item is not None:
                click(item)
            alert = wait(lambda: find(app, "alert"), 15)
            title = name(alert) if alert is not None else None
            text = " ".join(labels(alert)) if alert is not None else ""
            ok_btn = button(alert, "OK") if alert is not None else None
            if ok_btn is not None:
                click(ok_btn)
            on = wait(lambda: name(emu.area()).startswith("Type 'n Speak (English): keyboard"), 15)
            check("Type 'n Speak: first-start dialog", title == "Type 'n Speak (English)"
                  and "initialize file system" in text and on,
                  "alert %r: %r...; unit %s" % (title, text[:50], "on" if on else "NOT on"))
            emu.focus()
            time.sleep(1.0)
            mark = len(emu.lines())
            xdo("keydown", "y", "sleep", "0.05", "keyup", "y", "sleep", "0.2", "keydown", "F10", "sleep", "0.05",
                "keyup", "F10")
            time.sleep(0.5)
            got = [ln.split(" ", 1)[1] for ln in emu.lines()[mark:] if re.match(r"^[0-9.]+ tns ", ln)]
            menu_opened = "menu" in emu.lines()[mark:] or emu.menu_open()
            check("Type 'n Speak: y and F10 are its keys", got == ["tns 0xBD", "tns 0x3D", "tns 0xCE", "tns 0x4E"]
                  and not menu_opened, "%s%s" % (got, "; the MENU opened" if menu_opened else ""))
            mark = len(emu.lines())
            xdo("key", "alt+shift+f")
            first = wait(emu.menu_open, 5)
            tail = emu.lines()[mark:]
            let_go = [ln for ln in tail if "(let go)" in ln]
            time.sleep(0.5)
            emu.close_menu()
            check("Type 'n Speak: Alt+Shift+F, the menu", bool(first) and "menu" in tail and len(let_go) == 2,
                  "menu %s; held keys let go: %s" % ("open" if first else "NOT open",
                                                     [ln.split(" ", 1)[1] for ln in let_go]))
            emu.focus()

        if want("exit"):
            item = find(app, "menu item", "Exit")
            if item is not None:
                click(item)
            try:
                code = emu.p.wait(30)
            except subprocess.TimeoutExpired:
                code = None
            saved = sorted(f for f in os.listdir(emu.cfg) if f.endswith(".state"))
            check("Exit: the units saved", code == 0 and "english.state" in saved, "exit %s; saved %s" % (code, saved))
    finally:
        emu.stop()


def run_held(exe, fw, tmp):
    emu = Emu(exe, fw, tmp, "held", ["--unit", "bl-en", "--ram-has", "initialize file system"])
    try:
        if emu.connect() is None:
            check("i-chord held through the restart", False, "the program not on the accessibility bus")
            return
        emu.focus()
        emu.quiet_after_boot()
        # p-chord (dots 1 2 3 4 and space), then l (1 2 3), then i-chord (2 4 and space) held while the unit restarts
        # -- test_emu_linux.py's "held", here as the X server's keys going down and up
        seq = []
        for k in ("f", "d", "s", "j", "space"):
            seq += ["keydown", k, "sleep", "0.01"]
        seq += ["sleep", "0.05"]
        for k in ("f", "d", "s", "j", "space"):
            seq += ["keyup", k, "sleep", "0.01"]
        seq += ["sleep", "2.0"]
        for k in ("f", "d", "s"):
            seq += ["keydown", k, "sleep", "0.01"]
        seq += ["sleep", "0.05"]
        for k in ("f", "d", "s"):
            seq += ["keyup", k, "sleep", "0.01"]
        seq += ["sleep", "0.12"]
        for k in ("d", "j", "space"):
            seq += ["keydown", k, "sleep", "0.01"]
        seq += ["sleep", "1.75"]
        for k in ("d", "j", "space"):
            seq += ["keyup", k, "sleep", "0.01"]
        xdo(*seq)
        found = wait(lambda: [ln for ln in emu.lines() if "ram-has initialize file system" in ln], 8)
        held = []
        for ln in emu.lines():                  # the keys down as they changed (X's auto-repeat repeats them)
            m = re.match(r"^[0-9.]+ held (0x[0-9A-F]+)", ln)
            if m and (not held or held[-1] != m.group(1)):
                held.append(m.group(1))
        check("i-chord held through the restart", bool(found), "the unit asked \"initialize file system\": %s; "
              "keys down %s" % ("yes" if found else "no", " ".join(held)))
    finally:
        emu.stop()


def main():
    exe, fw = sys.argv[1], sys.argv[2]
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None

    def want(n):
        return only is None or n in only
    tmp = tempfile.mkdtemp(prefix="blazie_emu_gtk_test.")
    try:
        if any(want(n) for n in ("settings", "tree", "chord", "export", "menu", "dialog", "announce", "tns",
                                 "exit")):
            run_main(exe, fw, tmp, want)
        if want("held"):
            run_held(exe, fw, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("gtk emulator: %s" % ("%d of %d FAILED" % (failures, ran) if failures else "all %d passed" % ran))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
