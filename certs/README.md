# TLS Certificates

Sandal Core does not ship private keys or server certificates. If you run the server with TLS enabled (default on port 443 for gRPC / Auth services), you must provide your own certificates.

---

## Automatic Generation

On the first launch, `scripts/make_certs.ps1` (run by every launcher) creates:

- `ca.crt`: your own local CA, added to your Windows CurrentUser Trusted Root store (Windows asks you to confirm). The game client validates the server against it. The CA's private key is deleted right after signing.
- `server.pem` + `server.key`: the server certificate for `IP:127.0.0.1` and `DNS:localhost`, signed by that CA. The Go gateway reads them from here (`AOC_GATEWAY_TLS_CERT`, `AOC_GATEWAY_TLS_KEY`); the launchers copy them next to the C++ servers.

`powershell -File scripts/make_certs.ps1 -Force` makes new ones and replaces the old CA in the trust store. None of these files are committed (`.gitignore`).

---

## Manual Generation (OpenSSL)

To generate self-signed certificates manually:

```bash
# Generate private key and certificate in the certs/ directory
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout certs/server.key \
  -out certs/server.crt \
  -days 365 \
  -subj "/CN=localhost/O=AoC-Emu"
```

Or generate a combined PEM file:
```bash
cat certs/server.crt certs/server.key > certs/server.pem
```

The server inspects the following paths in order:
1. `certs/server.pem`
2. `certs/server.crt` and `certs/server.key`

---

## Disabling TLS for Local Testing

If you do not wish to use TLS during local development or debugging, start the server with the `--no-tls` flag:

```powershell
.\build\bin\aoc_server.exe --no-tls
```
