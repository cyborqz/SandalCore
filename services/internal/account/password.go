// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package account

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"runtime"
	"strconv"
	"strings"

	"golang.org/x/crypto/argon2"
	"golang.org/x/crypto/bcrypt"
)

// Passwords are stored as Argon2id in the PHC string format,
//
//	$argon2id$v=19$m=19456,t=2,p=1$<salt>$<key>
//
// (the salt and the key in base64 without padding) with 19 MiB of memory, 2 passes and 1 lane. An account made before this keeps a bcrypt hash ("$2a$...");
// it still signs in, and its hash is rewritten as Argon2id at its next successful sign-in.
const (
	argonMemoryKiB = 19 * 1024
	argonTime      = 2
	argonThreads   = 1
	argonSaltBytes = 16
	argonKeyBytes  = 32
)

// What a stored hash may ask for: a damaged row must not be able to ask for gigabytes or minutes.
const (
	argonMaxMemoryKiB = 256 * 1024
	argonMaxTime      = 10
	argonMaxThreads   = 16
)

var errUnreadableHash = errors.New("not a bcrypt or Argon2id hash")

// hashSlots bounds how many Argon2id hashes run at once: each holds its memory while it runs, so a burst of sign-ins waits for a slot instead of using gigabytes.
var hashSlots = make(chan struct{}, max(2, runtime.NumCPU()))

type argonParams struct {
	memory  uint32 // KiB
	time    uint32
	threads uint8
}

// current is the form of every new hash.
var current = argonParams{memory: argonMemoryKiB, time: argonTime, threads: argonThreads}

func argonKey(password, salt []byte, p argonParams, keyLen uint32) []byte {
	hashSlots <- struct{}{}
	defer func() { <-hashSlots }()
	return argon2.IDKey(password, salt, p.time, p.memory, p.threads, keyLen)
}

// hashPassword makes the stored form of a new password.
func hashPassword(password string) (string, error) {
	salt := make([]byte, argonSaltBytes)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	key := argonKey([]byte(password), salt, current, argonKeyBytes)
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s", argon2.Version, current.memory, current.time, current.threads,
		base64.RawStdEncoding.EncodeToString(salt), base64.RawStdEncoding.EncodeToString(key)), nil
}

// parseArgon2id reads the PHC string of an Argon2id hash. It refuses a hash that asks for more than the limits above.
func parseArgon2id(stored string) (p argonParams, salt, key []byte, err error) {
	bad := func(why string) (argonParams, []byte, []byte, error) {
		return argonParams{}, nil, nil, fmt.Errorf("argon2id hash: %s", why)
	}
	parts := strings.Split(stored, "$") // "", "argon2id", "v=19", "m=...,t=...,p=...", salt, key
	if len(parts) != 6 || parts[0] != "" || parts[1] != "argon2id" {
		return bad("the fields are not as expected")
	}
	if parts[2] != "v="+strconv.Itoa(argon2.Version) {
		return bad("version " + parts[2] + " is not read")
	}
	values := map[string]uint64{}
	for _, field := range strings.Split(parts[3], ",") {
		name, text, ok := strings.Cut(field, "=")
		n, perr := strconv.ParseUint(text, 10, 32)
		if !ok || perr != nil {
			return bad("the parameters do not read")
		}
		values[name] = n
	}
	m, t, th := values["m"], values["t"], values["p"]
	if len(values) != 3 || t < 1 || t > argonMaxTime || th < 1 || th > argonMaxThreads || m < 8*th || m > argonMaxMemoryKiB {
		return bad("the parameters are out of range")
	}
	salt, err = base64.RawStdEncoding.Strict().DecodeString(parts[4])
	if err != nil || len(salt) < 8 {
		return bad("the salt does not read")
	}
	key, err = base64.RawStdEncoding.Strict().DecodeString(parts[5])
	if err != nil || len(key) < 16 || len(key) > 64 {
		return bad("the key does not read")
	}
	return argonParams{memory: uint32(m), time: uint32(t), threads: uint8(th)}, salt, key, nil
}

// checkPassword compares a password with a stored hash. match says whether it is the password; upgrade says that the hash is not in today's form (a bcrypt hash, or an Argon2id
// one with weaker settings), so that the caller rewrites it after a match; err is set when the stored text is not a hash this build reads.
func checkPassword(stored, password string) (match, upgrade bool, err error) {
	switch {
	case strings.HasPrefix(stored, "$argon2id$"):
		p, salt, want, perr := parseArgon2id(stored)
		if perr != nil {
			return false, false, perr
		}
		got := argonKey([]byte(password), salt, p, uint32(len(want)))
		if subtle.ConstantTimeCompare(got, want) != 1 {
			return false, false, nil
		}
		return true, p.memory < current.memory || p.time < current.time || p.threads < current.threads, nil
	case strings.HasPrefix(stored, "$2a$"), strings.HasPrefix(stored, "$2b$"), strings.HasPrefix(stored, "$2y$"):
		berr := bcrypt.CompareHashAndPassword([]byte(stored), []byte(password))
		switch {
		case berr == nil:
			return true, true, nil
		case errors.Is(berr, bcrypt.ErrMismatchedHashAndPassword):
			return false, false, nil
		default:
			return false, false, berr
		}
	}
	return false, false, errUnreadableHash
}
