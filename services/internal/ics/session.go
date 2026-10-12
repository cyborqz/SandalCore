// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"context"
	"log/slog"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
)

// pingConfig is what the legacy server sends for the client's latency probes
// (the retail hosts; the client only pings them).
const pingConfig = `{"log_level":"info","ping_hosts":[{"host":"ping-eunorth1.ashesofcreation.com","name":"eunorth1"},{"host":"ping-useast2.ashesofcreation.com","name":"useast2"}],"ping_period_msec":2000,"version":"0.2.13"}`

const sessionReply = "ics_common.SessionReply"

// Cyb captures, C | O | A code

func (s *Server) onSession(ctx context.Context, sess *session, o *outbox) error {
	var req icscommon.SessionRequest
	if err := proto.Unmarshal(o.in.GetMessageData(), &req); err != nil {
		return o.reply(sessionReply, &icscommon.SessionReply{ResultCode: codeInvalidRequest}, codeInvalidRequest, "ics_session")
	}
	rep := &icscommon.SessionReply{ResultCode: success, RequestType: req.GetRequestType()}
	switch req.GetRequestType() {
	case icscommon.SessionRequestType_SESSION_REQUEST_TYPE_OPEN:
		return s.openSession(ctx, sess, o, rep)
	case icscommon.SessionRequestType_SESSION_REQUEST_TYPE_CLOSE:
		// The session ends; the stream may carry on.
		if sess.account != nil {
			slog.Info("ics: session closed", "session", sess.id, "username", sess.account.Username)
		}
		sess.end()
		return o.reply(sessionReply, rep, success, "ics_session")
	}
	// INVALID (the default) or a type this server does not know: the session
	// stays as it is.
	slog.Info("ics: session request of no known type", "session", sess.id, "type", int32(req.GetRequestType()))
	rep.ResultCode = codeInvalidRequest
	return o.reply(sessionReply, rep, codeInvalidRequest, "ics_session")
}

// openSession opens the session with the token in the wrapper's auth_token
// tag. It replaces any session the stream had: when the new one is refused,
// none is left.
func (s *Server) openSession(ctx context.Context, sess *session, o *outbox, rep *icscommon.SessionReply) error {
	sess.end()
	token := o.in.GetSystemData().GetTags()["auth_token"]
	acc, err := s.deps.Accounts.ValidateToken(ctx, token)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err() // the client left
		}
		code := codeInternal
		if status.Code(err) == codes.Unauthenticated {
			code = codeAuthFailed
			slog.Info("ics: session refused: not a valid login token (sign in through the launcher)", "session", sess.id)
		} else {
			slog.Error("ics: checking the login token failed", "session", sess.id, "err", err)
		}
		rep.ResultCode = code
		return o.reply(sessionReply, rep, code, "ics_session")
	}
	chars, err := s.deps.Characters.List(ctx, acc.ID)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		slog.Error("ics: listing characters failed", "session", sess.id, "err", err)
		rep.ResultCode = codeInternal
		return o.reply(sessionReply, rep, codeInternal, "ics_session")
	}
	sess.account, sess.token, sess.chars = &acc, token, len(chars)
	slog.Info("ics: session open", "session", sess.id, "username", acc.Username, "characters", len(chars))

	rep.DataMap = map[string]string{
		"ics_ping_client_config.json": pingConfig,
		// A serialized message in a string map. The Go runtime requires valid
		// UTF-8 there, which this is: character ids and names are ASCII, and
		// each mini record is far shorter than 128 bytes, so no length byte
		// reaches 0x80.
		"player_session_state": string(s.sessionState(chars)),
	}
	return o.reply(sessionReply, rep, success, "ics_session")
}

// onAuthRefresh answers the client's refresh (every 6 minutes) with the
// session's token, which stays its credential, once the auth service says it
// is still valid. Tokens are checked at OPEN and here: one that has expired
// ends the session. When the auth service cannot be reached nothing says the
// token expired, and the session carries on.
func (s *Server) onAuthRefresh(ctx context.Context, sess *session, o *outbox) error {
	const replyType, ftag = "ics_xclient.AuthRefreshReply", "ics_xclient_auth"
	if _, err := s.deps.Accounts.ValidateToken(ctx, sess.token); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if status.Code(err) != codes.Unauthenticated {
			slog.Error("ics: checking the login token failed", "session", sess.id, "err", err)
			return o.reply(replyType, nil, codeInternal, ftag)
		}
		slog.Info("ics: session ended: the login token has expired", "session", sess.id, "username", sess.account.Username)
		sess.end()
		return o.reply(replyType, nil, codeAuthFailed, ftag)
	}
	return o.reply(replyType, &icsxclient.AuthRefreshReply{AuthToken: sess.token, RefreshToken: sess.token}, success, ftag)
}

// sessionState is the serialized PlayerSessionState: one mini record per
// character.
func (s *Server) sessionState(chars []*characterv1.Character) []byte {
	state := &icsxclient.PlayerSessionState{}
	for _, c := range chars {
		state.CharacterMiniRecords = append(state.CharacterMiniRecords, &icsxclient.PlayerCharacterMiniRecord{
			CharacterId: c.GetId(), CharacterName: c.GetName(), WorldId: s.cfg.WorldID,
		})
	}
	b, _ := proto.Marshal(state) // ids and names are UTF-8: the character store checks them
	return b
}

// world is the one world the gateway offers, with the session's character
// count.
func (s *Server) world(sess *session) *icsxclient.WorldInfo {
	return &icsxclient.WorldInfo{
		WorldId: s.cfg.WorldID, WorldDisplayName: s.cfg.WorldName, Available: true, CharacterCount: int32(sess.chars),
	}
}

// onInterest pushes the world status when the client starts listening for it;
// stopping needs no answer.
func (s *Server) onInterest(sess *session, o *outbox) error {
	var ev icsxclient.GameClientInterestEvent
	if err := proto.Unmarshal(o.in.GetMessageData(), &ev); err != nil {
		slog.Info("ics: unreadable interest event", "session", sess.id, "err", err)
		return nil
	}
	if ev.GetStartInterestBitmask() == 0 {
		return nil
	}
	return o.push("ics_xclient.WorldStatusEvent",
		&icsxclient.WorldStatusEvent{Worlds: []*icsxclient.WorldInfo{s.world(sess)}}, "ics_lobby")
}
