// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
// main.go: the game master's dashboard: one window with the players, their
// stats, the living world and the cities. The bindings the window calls live
// in app.go, the window's own files in frontend\.
package main

import (
	"embed"
	"fmt"
	"log"
	"os"
	"path/filepath"
	goruntime "runtime"
	"syscall"
	"time"
	"unsafe"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	wailsrt "github.com/wailsapp/wails/v2/pkg/runtime"
)

//go:embed all:frontend
var assets embed.FS

// windowTitle: the title of the dashboard's window.
const windowTitle = "Sandal Core — لوحة تحكم الخادم"

// main: the window. The dashboard is a desktop window of its own, not a tab
// in a browser.
func main() {
	app := NewApp()
	err := wails.Run(&options.App{
		Title:            windowTitle,
		Width:            1360,
		Height:           900,
		AssetServer:      &assetserver.Options{Assets: assets},
		Bind:             []interface{}{app},
		OnStartup:        app.startup,
		OnShutdown:       app.shutdown,
		BackgroundColour: &options.RGBA{R: 18, G: 20, B: 26, A: 1},
	})
	if err != nil {
		log.Fatal(err)
	}
}

// ── the INSERT key brings the window back ────────────────────────────────
// The game runs full screen, so the dashboard ends up behind it. A global
// hotkey (INSERT) raises the window over the game; the next press puts the
// game back in front.
//
// The window is driven through Win32 directly, not through the window
// library: the game holds the full screen, so the dashboard has to be put
// on top of it (HWND_TOPMOST) and not only brought to the front.
const (
	hotkeyID    = 1
	vkInsert    = 0x2D
	modNoRepeat = 0x4000
	wmHotkey    = 0x0312

	swHide    = 0
	swShow    = 5
	swRestore = 9

	swpNoSize     = 0x0001
	swpNoMove     = 0x0002
	swpNoActivate = 0x0010
	swpShowWindow = 0x0040
)

var (
	user32               = syscall.NewLazyDLL("user32.dll")
	procRegisterHotkey   = user32.NewProc("RegisterHotKey")
	procUnregisterHotkey = user32.NewProc("UnregisterHotKey")
	// user32 exports GetMessageA and GetMessageW only; there is no plain
	// GetMessage to load.
	procGetMessage         = user32.NewProc("GetMessageW")
	procEnumWindows        = user32.NewProc("EnumWindows")
	procGetWindowThreadPid = user32.NewProc("GetWindowThreadProcessId")
	procIsWindow           = user32.NewProc("IsWindow")
	procIsWindowVisible    = user32.NewProc("IsWindowVisible")
	procShowWindow         = user32.NewProc("ShowWindow")
	procSetWindowPos       = user32.NewProc("SetWindowPos")
	procBringWindowToTop   = user32.NewProc("BringWindowToTop")
	procSetForeground      = user32.NewProc("SetForegroundWindow")
	procGetWindowText      = user32.NewProc("GetWindowTextW")
	procGetWindowTextLen   = user32.NewProc("GetWindowTextLengthW")

	// HWND_TOPMOST and HWND_NOTOPMOST are (HWND)-1 and (HWND)-2.
	hwndTopmost   = ^uintptr(0)
	hwndNotopmost = ^uintptr(1)
)

// winMessage: the MSG structure of Windows (64 bits: 48 bytes).
type winMessage struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	pt      struct{ x, y int32 }
}

// enumCallback: the callback of EnumWindows. It is kept in a variable so the
// garbage collector does not take it while Windows still holds its address.
var enumCallback = syscall.NewCallback(func(hwnd uintptr, _ uintptr) uintptr {
	if findWindow == nil {
		return 1
	}
	if findWindow(hwnd) {
		return 0 // stop: the window is found
	}
	return 1
})

// findWindow: the test EnumWindows runs on every window it walks.
var findWindow func(hwnd uintptr) bool

// watchHotkey: waits for the INSERT key and raises the window over the game.
//
// The hotkey belongs to the thread that registers it, so the thread is locked:
// Go would otherwise move the goroutine to another thread between the
// registration and the wait, and the key would land in a queue nobody reads.
func (a *App) watchHotkey() {
	goruntime.LockOSThread()
	defer goruntime.UnlockOSThread()

	registered, _, _ := procRegisterHotkey.Call(0, hotkeyID, modNoRepeat, vkInsert)
	if registered == 0 {
		a.logf("the INSERT key is taken by another program")
		return // another program holds the key; the window still comes forward by hand
	}
	defer procUnregisterHotkey.Call(0, hotkeyID)
	a.logf("the INSERT key is ours")

	var msg winMessage
	for {
		got, _, _ := procGetMessage.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(got) == -1 {
			a.logf("the message loop stopped")
			return
		}
		if msg.message == wmHotkey {
			a.logf("a hotkey arrived (id=%d)", msg.wParam)
			if msg.wParam == hotkeyID {
				a.toggleWindow()
			}
			continue
		}
	}
}

// toggleWindow: the INSERT key. The first press brings the dashboard over the
// game and holds it there; the next one hides the dashboard and gives the
// screen back to the game.
func (a *App) toggleWindow() {
	hwnd := a.windowHandle()
	if hwnd == 0 {
		a.logf("the dashboard's window is not there")
		return
	}

	a.windowMu.Lock()
	raised := a.windowRaised
	a.windowRaised = !raised
	a.windowMu.Unlock()

	if raised {
		// the dashboard was in front: hide it and give the game the screen
		procSetWindowPos.Call(hwnd, hwndNotopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpNoActivate)
		procShowWindow.Call(hwnd, swHide)
		a.logf("the dashboard is away")
		a.notify("اللوحة اختفت — اللعبة رجعت")
		return
	}

	// the dashboard comes forward. The game is full screen, so the window has
	// to sit above it, not only in front of it.
	procShowWindow.Call(hwnd, swRestore)
	procShowWindow.Call(hwnd, swShow)
	procSetWindowPos.Call(hwnd, hwndTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procBringWindowToTop.Call(hwnd)
	procSetForeground.Call(hwnd)
	a.logf("the dashboard is up")
	a.notify("لوحة التحكم ظهرت — INSERT يخفيها")
}

// windowHandle: the dashboard's own window. The window library keeps the
// handle to itself, and the hotkey needs it to put the window above the full
// screen game. The handle is found by the title the window was given.
func (a *App) windowHandle() uintptr {
	a.handleMu.Lock()
	defer a.handleMu.Unlock()

	if a.hwnd != 0 {
		if alive, _, _ := procIsWindow.Call(a.hwnd); alive != 0 {
			return a.hwnd
		}
	}
	a.hwnd = 0

	pid := uint32(os.Getpid())
	findWindow = func(hwnd uintptr) bool {
		var owner uint32
		procGetWindowThreadPid.Call(hwnd, uintptr(unsafe.Pointer(&owner)))
		if owner != pid {
			return false
		}
		if windowTitleOf(hwnd) != windowTitle {
			return false
		}
		a.hwnd = hwnd
		return true
	}
	procEnumWindows.Call(enumCallback, 0)
	findWindow = nil

	return a.hwnd
}

// windowTitleOf: the title of a window.
func windowTitleOf(hwnd uintptr) string {
	length, _, _ := procGetWindowTextLen.Call(hwnd)
	if length == 0 {
		return ""
	}
	buf := make([]uint16, int(length)+1)
	procGetWindowText.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return syscall.UTF16ToString(buf)
}

// notify: a message for the window's toast.
func (a *App) notify(message string) {
	if a.ctx == nil {
		return
	}
	wailsrt.EventsEmit(a.ctx, "toast", message)
}

// logf: a line in the window's own log, so the hotkey can be followed without
// a console (the window is built without one).
func (a *App) logf(format string, args ...any) {
	path := filepath.Join(os.TempDir(), "aocadmin.log")
	line := time.Now().Format("15:04:05") + " " + fmt.Sprintf(format, args...) + "\n"
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	f.WriteString(line)
}
