// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"unicode"
)

// The roster is the 46 city nodes the retail game state carries (capture 1198, `wire.md` section 9): the five Riverlands nodes
// and the starting nodes, the other regions' nodes, the harbours, the artisan POIs and the lawless ocean.  Each is a
// CityNodeRecordBase of the design DB, named as content.db names it.  roster.json lists them by guid; a test checks every
// guid and name against the real content.db when there is one.
//
// A node's rank (the key of the client's small node maps) is not here: it is the index in the guid-sorted list of ALL 81
// CityNodeRecordBase rows, and contentc's node tables (Nodes 1, part B) carry it.

//go:embed roster.json
var rosterJSON []byte

// Info is a node the service knows: its record guid and names.
type Info struct {
	Guid        int64  `json:"guid"`
	Name        string `json:"name"`         // the CityNodeRecordBase name, "Verra_RVR_Joeva": what the client's GM commands take
	DisplayName string `json:"display_name"` // the record's nodeName, "Joeva"
}

// Roster is the nodes in guid order, with the ways to find one.
type Roster struct {
	nodes  []Info
	byGuid map[int64]int
	byName map[string]int // normalised (norm): the record name, the record name without "Verra_", the display name
}

// DefaultRoster is the embedded roster.json.
func DefaultRoster() *Roster {
	r, err := parseRoster(rosterJSON)
	if err != nil {
		panic("citynode: roster.json: " + err.Error()) // a build error: the embedded file is tested
	}
	return r
}

// (C.Y.B | CoA)

func parseRoster(b []byte) (*Roster, error) {
	var nodes []Info
	if err := json.Unmarshal(b, &nodes); err != nil {
		return nil, err
	}
	sort.Slice(nodes, func(i, j int) bool { return nodes[i].Guid < nodes[j].Guid })
	r := &Roster{nodes: nodes, byGuid: map[int64]int{}, byName: map[string]int{}}
	for i, n := range nodes {
		if n.Guid <= 0 || n.Name == "" || n.DisplayName == "" {
			return nil, fmt.Errorf("node %d is incomplete: %+v", i, n)
		}
		if _, dup := r.byGuid[n.Guid]; dup {
			return nil, fmt.Errorf("guid %d is listed twice", n.Guid)
		}
		r.byGuid[n.Guid] = i
		for _, key := range keys(n) {
			if j, dup := r.byName[key]; dup && j != i {
				return nil, fmt.Errorf("the name %q belongs to %s and %s", key, nodes[j].Name, n.Name)
			}
			r.byName[key] = i
		}
	}
	return r, nil
}

// keys are the names a node answers to, normalised (norm).
func keys(n Info) []string {
	out := []string{norm(n.Name), norm(n.DisplayName)}
	if short := strings.TrimPrefix(n.Name, "Verra_"); short != n.Name {
		out = append(out, norm(short))
	}
	return out
}

// norm is a name lower-cased with everything but letters and digits taken out, so "New Aela", "NewAela" and "new_aela" are one name.
func norm(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if unicode.IsLetter(r) || unicode.IsDigit(r) {
			b.WriteRune(r)
		}
	}
	return b.String()
}

// Nodes are the roster's nodes in guid order. The caller must not change them.
func (r *Roster) Nodes() []Info { return r.nodes }

// Len is the number of nodes.
func (r *Roster) Len() int { return len(r.nodes) }

// ByGuid finds a node by its record guid.
func (r *Roster) ByGuid(guid int64) (Info, bool) {
	i, ok := r.byGuid[guid]
	if !ok {
		return Info{}, false
	}
	return r.nodes[i], true
}

// Resolve finds a node by the way a caller names it: a guid (non-zero guid wins), else a name: the record name
// ("Verra_RVR_Joeva"), the same without "Verra_", or the display name ("Joeva"), in any letter case and ignoring spaces,
// underscores and punctuation ("New Aela", "NewAela" and "new_aela" are one node), or a guid written in decimal.  Nothing found is ErrNotFound; neither given is ErrInvalid.
func (r *Roster) Resolve(guid int64, name string) (Info, error) {
	if guid != 0 {
		if n, ok := r.ByGuid(guid); ok {
			return n, nil
		}
		return Info{}, fmt.Errorf("%w: no city node has the guid %d", ErrNotFound, guid)
	}
	name = strings.TrimSpace(name)
	if name == "" {
		return Info{}, fmt.Errorf("%w: a node needs a guid or a name", ErrInvalid)
	}
	if i, ok := r.byName[norm(name)]; ok {
		return r.nodes[i], nil
	}
	if g, err := strconv.ParseInt(name, 10, 64); err == nil {
		if n, ok := r.ByGuid(g); ok {
			return n, nil
		}
	}
	return Info{}, fmt.Errorf("%w: no city node is named %q (try nodectl list for the names)", ErrNotFound, name)
}
