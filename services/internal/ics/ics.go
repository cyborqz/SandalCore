// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package ics serves the game client's ICS XClient stream: the one gRPC
// method the client calls (/ics_xclient.XClientService/ProcessXClientMessage),
// a bidirectional stream of ics_common.MessageWrapper carrying its session,
// world, character and play requests. The replies follow the legacy emulator
// (src/services/xclient/xclient_service.h), which the client is proven to
// accept.
package ics

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"sort"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"google.golang.org/grpc/metadata"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
)

// Account is who a session belongs to.
type Account struct {
	ID       int64
	Username string
}

// Accounts resolves the token the client presents (the launcher's login
// token, which the tether handed it) to its account. An unknown or expired
// token is a gRPC Unauthenticated error.
type Accounts interface {
	ValidateToken(ctx context.Context, token string) (Account, error)
}

// Characters is the character service as the gateway uses it. Errors are
// gRPC status errors (NotFound, AlreadyExists, InvalidArgument, ...).
type Characters interface {
	List(ctx context.Context, accountID int64) ([]*characterv1.Character, error)
	Get(ctx context.Context, id string) (*characterv1.Character, error)
	Create(ctx context.Context, req *characterv1.CreateCharacterRequest) (*characterv1.Character, error)
	Delete(ctx context.Context, accountID int64, id string) error
}

// Placer chooses the world server a character joins. A gRPC Unavailable error
// means there is none; any other error is a failure to find out.
type Placer interface {
	Place(ctx context.Context, characterID string) (serverID uint32, address string, err error)
}

// Tickets issues one-time join tickets (32 lowercase hex digits).
// tickets.ErrInvalid means the character is not the account's (any more).
type Tickets interface {
	Issue(ctx context.Context, accountID int64, characterID string, serverID uint32) (string, error)
}

// Cyb with C | O | A

// Deps are the services behind the stream.
type Deps struct {
	Accounts   Accounts
	Characters Characters
	Placer     Placer
	Tickets    Tickets
}

// Config describes the one world the gateway offers, its pacing and limits.
type Config struct {
	WorldID       string        // the client remembers the world it last chose by this id
	WorldName     string        // shown in the world list
	ServerTag     string        // the server_id tag on every message sent
	PlayPause     time.Duration // between the three writes that answer a PlayRequest
	MaxCharacters int           // an account with this many characters cannot create another
}

// DefaultConfig matches the legacy server, so a client that last played there
// keeps its world selection.
func DefaultConfig() Config {
	return Config{
		WorldID:       "aoc-release-useast2-local",
		WorldName:     "[EMU] Local Server",
		ServerTag:     "aoc-gateway",
		PlayPause:     400 * time.Millisecond,
		MaxCharacters: 16,
	}
}

// Server implements icsxclient.XClientServiceServer.
type Server struct {
	icsxclient.UnimplementedXClientServiceServer
	deps        Deps
	cfg         Config
	nextSession atomic.Uint64
}

func New(deps Deps, cfg Config) *Server {
	s := &Server{deps: deps, cfg: cfg}
	s.nextSession.Store(10000) // the legacy server's session ids start here too
	return s
}

// session is one client stream.
type session struct {
	id      uint64
	account *Account // nil until a SessionRequest(OPEN) with a valid token
	token   string   // the token that opened it
	chars   int      // the account's character count, for the world list and the limit
}

// end forgets the account: the stream carries on without a session.
func (sess *session) end() {
	sess.account, sess.token, sess.chars = nil, "", 0
}

// Bounds on what the client sends.
const (
	maxTypeName      = 128      // bytes of a message_type_name; the longest real one is about 40
	maxCreateInfo    = 64 << 10 // bytes of create_character_info_raw; the client's are about 6.5 KB
	maxCharacterName = 64       // bytes of a character name
	maxLogged        = 64       // bytes of a client-supplied string that go into the log
)

// ProcessXClientMessage serves one client for as long as its stream lives.
// Messages are answered one at a time, in order, as the legacy server does.
func (s *Server) ProcessXClientMessage(stream icsxclient.XClientService_ProcessXClientMessageServer) error {
	ctx := stream.Context()
	sess := &session{id: s.nextSession.Add(1)}
	md, _ := metadata.FromIncomingContext(ctx)
	slog.Info("ics stream opened", "session", sess.id, "metadata", metadataKeys(md))
	defer slog.Info("ics stream closed", "session", sess.id)
	for {
		in, err := stream.Recv()
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
		out := &outbox{send: stream.Send, in: in, session: sess, serverTag: s.cfg.ServerTag}
		if err := s.handle(ctx, sess, out); err != nil {
			return err
		}
	}
}

// built by C | Y | B with CoA

// handle answers one message. It returns an error only when the stream is
// broken; what the client got wrong is answered with a status code.
func (s *Server) handle(ctx context.Context, sess *session, o *outbox) error {
	if n := len(o.in.GetMessageTypeName()); n > maxTypeName {
		// No message type has a name this long; do not echo it.
		slog.Info("ics: message type name too long", "session", sess.id, "bytes", n, "type", clip(o.in.GetMessageTypeName()))
		return o.reply("", nil, codeInvalidRequest, "")
	}
	switch o.in.GetMessageTypeName() {
	case "ics_common.SessionRequest":
		return s.onSession(ctx, sess, o)
	case "ics_common.KeepAliveMessage":
		return o.reply("ics_common.KeepAliveMessage",
			&icscommon.KeepAliveMessage{TimestampMsec: time.Now().UnixMilli()}, success, "ics_keep_alive")
	}
	if sess.account == nil {
		slog.Info("ics: request without a session", "session", sess.id, "type", clip(o.in.GetMessageTypeName()))
		return o.reply(o.in.GetMessageTypeName(), nil, codeSessionCheckFailed, "")
	}
	switch o.in.GetMessageTypeName() {
	case "ics_xclient.CheckGameVersionRequest":
		// Any version is accepted; a non-empty message would show a popup.
		return o.reply("ics_xclient.CheckGameVersionReply", &icsxclient.CheckGameVersionReply{Supported: true}, success, "ics_lobby")
	case "ics_xclient.GetWorldsRequest":
		return o.reply("ics_xclient.GetWorldsReply",
			&icsxclient.GetWorldsReply{Worlds: []*icsxclient.WorldInfo{s.world(sess)}}, success, "ics_lobby")
	case "ics_xclient.GameClientInterestEvent":
		return s.onInterest(sess, o)
	case "ics_xclient.AuthRefreshRequest":
		return s.onAuthRefresh(ctx, sess, o)
	case "ics_xclient.GetCharactersRequest":
		return s.onGetCharacters(ctx, sess, o)
	case "ics_xclient.CreateCharacterRequest":
		return s.onCreateCharacter(ctx, sess, o)
	case "ics_xclient.DeleteCharacterRequest":
		return s.onDeleteCharacter(ctx, sess, o)
	case "ics_xclient.PlayRequest":
		return s.onPlay(ctx, sess, o)
	case "ics_xclient.PlayEndRequest":
		return o.reply("ics_xclient.PlayEndReply", &icsxclient.PlayEndReply{}, success, "ics_play_character")
	case "ics_xclient.GetShopHandoffCodeRequest":
		return o.reply("ics_xclient.GetShopHandoffCodeReply", &icsxclient.GetShopHandoffCodeReply{}, success, "")
	}
	// The legacy server answers anything else with an empty message of the
	// same type and SUCCESS. The client has no handlers for the ones it never
	// sends (CheckCharacterName, SelectCharacter, CanDeleteCharacter).
	slog.Warn("ics: unhandled message", "session", sess.id, "type", clip(o.in.GetMessageTypeName()))
	return o.reply(o.in.GetMessageTypeName(), nil, success, "")
}

// metadataKeys lists the gRPC metadata keys the client sent (not the values,
// which may carry credentials), each clipped.
func metadataKeys(md metadata.MD) []string {
	keys := make([]string, 0, len(md))
	for k := range md {
		keys = append(keys, clip(k))
	}
	sort.Strings(keys)
	return keys
}

// clip bounds a string the client sent for the log: at most maxLogged bytes,
// cut before a character that would not fit whole, and marked when cut.
func clip(s string) string {
	if len(s) <= maxLogged {
		return s
	}
	n := maxLogged
	for n > 0 && !utf8.RuneStart(s[n]) {
		n--
	}
	return fmt.Sprintf("%s...(%d bytes)", s[:n], len(s))
}

// failed answers a service call that failed: with no body and the status
// codeFor maps err to, or, when the stream has ended, not at all. The call
// then failed because the client left, which is no error, and returning the
// stream's error ends the handler.
func failed(ctx context.Context, o *outbox, replyType, what string, err error, ftag string) error {
	if ctx.Err() != nil {
		return ctx.Err()
	}
	return o.reply(replyType, nil, codeFor(o.session, what, err), ftag)
}
