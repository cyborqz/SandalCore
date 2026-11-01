// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks by Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// main.go: the game master's dashboard: one window with the players, their
// stats, the living world and the cities. The bindings the window calls live
// in app.go, the window's own files in frontend\.
package main

import (
	"embed"
	"log"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

//go:embed all:frontend
var assets embed.FS

// main: the window. The dashboard is a desktop window of its own, not a tab
// in a browser.
func main() {
	app := NewApp()
	err := wails.Run(&options.App{
		Title:            "Sandal Core — لوحة تحكم الخادم",
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
