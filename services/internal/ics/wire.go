// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package ics

import (
	"crypto/rand"
	"encoding/hex"
	"strconv"
	"time"

	"google.golang.org/protobuf/proto"

	icscommon "aocrop/services/gen/ics/common"
)

// Status codes the gateway answers with. Their numbers are the game
// executable's own (proto/ics_common.proto, golden-tested against
// testdata/ics_status_codes_exe.txt): a result is 0-3 and an error 10-127. The
// client's call machinery treats a number outside that as no answer, so a
// refused request would leave it waiting for the call to expire.
const (
	success                = icscommon.IcsStatusCode_ICS_STATUS_CODE_SUCCESS
	acknowledged           = icscommon.IcsStatusCode_ICS_STATUS_CODE_ACKNOWLEDGED
	codeInternal           = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_INTERNAL_ERROR
	codeInvalidRequest     = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_INVALID_REQUEST
	codeAuthFailed         = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_AUTHENTICATION_FAILED
	codeSessionCheckFailed = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_SESSION_CHECK_FAILED
	codeNameTaken          = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_EXISTING_NAME_FOUND
	codeCharacterNotFound  = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_CHARACTER_REC_NOT_FOUND
	codeNoServer           = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_PLAY_RESERVATION_NO_SERVER
	codeCharacterCount     = icscommon.IcsStatusCode_ICS_STATUS_CODE_ERROR_CHARACTER_COUNT_CHECK_FAILED
)

// outbox writes the answers to one incoming message.
type outbox struct {
	send      func(*icscommon.MessageWrapper) error
	in        *icscommon.MessageWrapper
	session   *session
	serverTag string
}

// reply answers the incoming message. It echoes the request's system_data
// (the client matches replies by its client_tracking_id tag) and adds the
// server's tags. A nil body is an empty message.
func (o *outbox) reply(typeName string, body proto.Message, code icscommon.IcsStatusCode, ftag string) error {
	data, err := marshal(body)
	if err != nil {
		return err
	}
	return o.write(typeName, data, code, ftag, true)
}

// push sends a message the client did not ask for; it echoes nothing.
func (o *outbox) push(typeName string, body proto.Message, ftag string) error {
	data, err := marshal(body)
	if err != nil {
		return err
	}
	return o.write(typeName, data, success, ftag, false)
}

// [C | O | A - C.Y.B]

func (o *outbox) write(typeName string, data []byte, code icscommon.IcsStatusCode, ftag string, echo bool) error {
	sd := &icscommon.SystemData{}
	if echo && o.in.GetSystemData() != nil {
		sd = proto.Clone(o.in.GetSystemData()).(*icscommon.SystemData)
	}
	if sd.Tags == nil {
		sd.Tags = map[string]string{}
	}
	if ftag != "" {
		sd.Tags["ftag"] = ftag
	}
	sd.Tags["server_id"] = o.serverTag
	sd.Tags["msg_uuid"] = randomHex()
	sd.Tags["server_session_id"] = strconv.FormatUint(o.session.id, 10)
	sd.Tags["server_epoch_nanosec"] = strconv.FormatInt(time.Now().UnixNano(), 10)
	return o.send(&icscommon.MessageWrapper{SystemData: sd, StatusCode: code, MessageTypeName: typeName, MessageData: data})
}

func marshal(body proto.Message) ([]byte, error) {
	if body == nil {
		return nil, nil
	}
	return proto.Marshal(body)
}

// randomHex is 32 random lowercase hex digits, like the legacy msg_uuid.
func randomHex() string {
	var b [16]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}
