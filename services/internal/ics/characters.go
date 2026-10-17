// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"context"
	"log/slog"
	"regexp"
	"strings"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/aocchar"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/createinfo"
)

// onGetCharacters answers any world_id with the account's characters: there
// is one world.
func (s *Server) onGetCharacters(ctx context.Context, sess *session, o *outbox) error {
	const replyType = "ics_xclient.GetCharactersReply"
	chars, err := s.deps.Characters.List(ctx, sess.account.ID)
	if err != nil {
		return failed(ctx, o, replyType, "listing characters", err, "")
	}
	rep := &icsxclient.GetCharactersReply{}
	for _, c := range chars {
		rep.CharactersRaw = append(rep.CharactersRaw, aocchar.Record(c, s.cfg.WorldID))
	}
	sess.chars = len(chars)
	return o.reply(replyType, rep, success, "")
}

// ~ C_Y_B :: C | O | A ~

// onCreateCharacter creates a character from the client's CreateCharacterInfo
// payload: gender from field 2, race from field 3 and class from field 4 (the
// client's enums; a zero is absent from the wire, so a Kaelar male sends
// neither of the first two), the customization JSON as the appearance, and the
// whole payload kept as create_info. The character service checks all of it
// (it refuses a race, gender, class or customization the design data does not
// allow, and a JSON whose race, gender or class names disagree with the
// fields), so this only reads them: a refusal is an INVALID_REQUEST.
func (s *Server) onCreateCharacter(ctx context.Context, sess *session, o *outbox) error {
	const replyType = "ics_xclient.CreateCharacterReply"
	var req icsxclient.CreateCharacterRequest
	if err := proto.Unmarshal(o.in.GetMessageData(), &req); err != nil {
		return o.reply(replyType, nil, codeInvalidRequest, "")
	}
	// The payload is stored twice (as create_info, and its JSON as the
	// appearance), and every character comes back in each list: a few huge
	// ones would push the account's list past gRPC's 4 MB and lock it out.
	if len(req.GetCreateCharacterInfoRaw()) > maxCreateInfo || len(req.GetCharacterName()) > maxCharacterName {
		slog.Info("ics: character not created: payload or name too long", "session", sess.id,
			"payload_bytes", len(req.GetCreateCharacterInfoRaw()), "name_bytes", len(req.GetCharacterName()))
		return o.reply(replyType, nil, codeInvalidRequest, "")
	}
	if sess.chars >= s.cfg.MaxCharacters {
		slog.Info("ics: character not created: the account has as many as it may", "session", sess.id,
			"characters", sess.chars)
		return o.reply(replyType, nil, codeCharacterCount, "")
	}
	info := createinfo.Parse(req.GetCreateCharacterInfoRaw())
	class, _ := chardef.ClassName(info.Class) // "" for a value that is no class: the character service refuses it
	c, err := s.deps.Characters.Create(ctx, &characterv1.CreateCharacterRequest{
		AccountId:      sess.account.ID,
		Name:           req.GetCharacterName(),
		RaceId:         info.Race,
		GenderId:       info.Gender,
		ClassId:        info.Class,
		ArchetypeId:    chardef.ArchetypeID(info.Class),
		ClassName:      class,
		AppearanceJson: info.Appearance,
		CreateInfo:     req.GetCreateCharacterInfoRaw(),
		Level:          1,
	})
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err() // the client left
		}
		code := codeFor(sess, "creating a character", err)
		slog.Info("ics: character not created", "session", sess.id, "name", clip(req.GetCharacterName()), "status", code.String())
		return o.reply(replyType, nil, code, "")
	}
	sess.chars++
	slog.Info("ics: character created", "session", sess.id, "username", sess.account.Username,
		"name", c.GetName(), "class", class)
	return o.reply(replyType, &icsxclient.CreateCharacterReply{
		CharacterId:      c.GetId(),
		CharacterName:    c.GetName(),
		WorldId:          s.cfg.WorldID,
		CharacterInfoRaw: aocchar.CreateReplyInfo(c),
	}, success, "")
}

// onDeleteCharacter deletes one of the account's characters. A character that
// is not there (already deleted, say by a double click, or another account's)
// is answered with SUCCESS, as the legacy server did: no error pops up, and
// the reply does not tell whether the id exists on another account.
func (s *Server) onDeleteCharacter(ctx context.Context, sess *session, o *outbox) error {
	const replyType = "ics_xclient.DeleteCharacterReply"
	var req icsxclient.DeleteCharacterRequest
	if err := proto.Unmarshal(o.in.GetMessageData(), &req); err != nil {
		return o.reply(replyType, nil, codeInvalidRequest, "")
	}
	id, ok := characterID(req.GetCharacterId())
	if !ok {
		return o.reply(replyType, nil, codeCharacterNotFound, "")
	}
	// The id as the client sent it, as the legacy server answers.
	rep := &icsxclient.DeleteCharacterReply{CharacterId: req.GetCharacterId()}
	err := s.deps.Characters.Delete(ctx, sess.account.ID, id)
	switch {
	case err == nil:
	case status.Code(err) == codes.NotFound:
		slog.Info("ics: character to delete not found", "session", sess.id, "character_id", id)
		return o.reply(replyType, rep, success, "")
	default:
		return failed(ctx, o, replyType, "deleting a character", err, "")
	}
	if sess.chars > 0 {
		sess.chars--
	}
	slog.Info("ics: character deleted", "session", sess.id, "username", sess.account.Username, "character_id", id)
	return o.reply(replyType, rep, success, "")
}

// hex32 is the shape of the character ids the services use.
var hex32 = regexp.MustCompile(`^[0-9a-f]{32}$`)

// characterID is a character id from the client as the services use it:
// lowercased (the client may print its GUIDs in upper case), and ok only when
// it is then 32 hex digits.
func characterID(fromClient string) (id string, ok bool) {
	id = strings.ToLower(fromClient)
	return id, hex32.MatchString(id)
}

// codeFor maps an error from the character service to the status the client
// gets, and logs the ones that are not the client's doing.
func codeFor(sess *session, what string, err error) icscommon.IcsStatusCode {
	switch status.Code(err) {
	case codes.ResourceExhausted:
		return codeCharacterCount
	case codes.AlreadyExists:
		return codeNameTaken
	case codes.InvalidArgument:
		return codeInvalidRequest
	case codes.NotFound:
		return codeCharacterNotFound
	}
	slog.Error("ics: "+what+" failed", "session", sess.id, "err", err)
	return codeInternal
}
