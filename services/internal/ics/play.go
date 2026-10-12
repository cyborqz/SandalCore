// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"context"
	"errors"
	"log/slog"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/tickets"
)

// onPlay sends the client to a world server, answering as the legacy and
// retail servers do:
//   - A request that cannot be played (it does not decode, or its id is not a
//     character id) gets one PlayReply with the error.
//   - Otherwise a PlayReply ACKNOWLEDGED with no body goes out at once, before
//     any lookup (those servers acknowledge within about 60 ms).
//   - Then the character must be the account's, the placer chooses the world
//     server and a join ticket is issued. A failure there is one more
//     PlayReply with the error and no body.
//   - On success, after a pause a pushed PlayRequestStatusReport (queue
//     position 0), and after another the PlayReply SUCCESS with the address
//     and the ticket.
//
// The client connects to the address and presents the character id and the
// ticket in its first UDP handshake packet.
func (s *Server) onPlay(ctx context.Context, sess *session, o *outbox) error {
	const replyType = "ics_xclient.PlayReply"
	const ftag = "ics_play_character"
	var req icsxclient.PlayRequest
	if err := proto.Unmarshal(o.in.GetMessageData(), &req); err != nil {
		return o.reply(replyType, nil, codeInvalidRequest, ftag)
	}
	id, ok := characterID(req.GetCharacterId())
	if !ok {
		return o.reply(replyType, nil, codeCharacterNotFound, ftag)
	}
	if err := o.reply(replyType, nil, acknowledged, ftag); err != nil {
		return err
	}

	c, err := s.deps.Characters.Get(ctx, id)
	if err == nil && c.GetAccountId() != sess.account.ID {
		err = status.Error(codes.NotFound, "not this account's character") // looks like a missing one
	}
	if err != nil {
		return failed(ctx, o, replyType, "loading the character to play", err, ftag)
	}
	serverID, address, err := s.deps.Placer.Place(ctx, c.GetId())
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err() // the client left
		}
		code := codeNoServer
		if status.Code(err) == codes.Unavailable {
			slog.Warn("ics: no world server to play on", "session", sess.id, "err", err)
		} else {
			code = codeInternal
			slog.Error("ics: placement failed", "session", sess.id, "err", err)
		}
		return o.reply(replyType, nil, code, ftag)
	}
	ticket, err := s.deps.Tickets.Issue(ctx, sess.account.ID, c.GetId(), serverID)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		code := codeInternal
		if errors.Is(err, tickets.ErrInvalid) {
			// Deleted (by another client of the account) since it was looked up.
			code = codeCharacterNotFound
			slog.Info("ics: character deleted while joining", "session", sess.id, "character_id", c.GetId())
		} else {
			slog.Error("ics: issuing a join ticket failed", "session", sess.id, "err", err)
		}
		return o.reply(replyType, nil, code, ftag)
	}

	if err := s.pause(ctx); err != nil {
		return err
	}
	if err := o.push("ics_xclient.PlayRequestStatusReport", &icsxclient.PlayRequestStatusReport{QueuePosition: 0}, ftag); err != nil {
		return err
	}
	if err := s.pause(ctx); err != nil {
		return err
	}
	slog.Info("ics: play", "session", sess.id, "username", sess.account.Username, "character", c.GetName(),
		"server_id", serverID, "address", address)
	return o.reply(replyType, &icsxclient.PlayReply{GameConnectionToken: ticket, GameConnectionString: address}, success, ftag)
}

// -- C-O-A & Cyb --

// pause waits PlayPause, or until the stream ends.
func (s *Server) pause(ctx context.Context) error {
	if s.cfg.PlayPause <= 0 {
		return nil
	}
	t := time.NewTimer(s.cfg.PlayPause)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-t.C:
		return nil
	}
}
