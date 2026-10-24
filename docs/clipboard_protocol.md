# Clipboard Protocol

Sunshine can share the clipboard with a streaming client in both directions: text, rich text (HTML and
RTF), PNG images, and files and folders. This page specifies version 1 of the wire protocol and the
behaviour a client must implement. Everything marked as a rule is normative for host and client.

The protocol is private to this Sunshine build and the matching client build. It is not part of the
GameStream protocol and is not coordinated with upstream Sunshine or Moonlight.

## Overview

- The channel is an HTTP/1.1 Upgrade on the existing nvhttp HTTPS port (47984 by default), so it reuses
  the client's paired certificate and needs no new port or firewall rule.
- A client may open the channel only while it has a running stream with the host, using the same
  paired certificate.
- Copying only announces the list of available formats. Bytes move only when the receiver requests
  them (announce, then pull). Each receiver decides when to pull: at once (prefetch) or when something
  pastes.
- The host never relays one client's clipboard to another client. Only the host's own clipboard is
  offered to clients.
- Control messages are JSON. Bulk data travels in binary DATA frames under per-stream credit flow
  control.

## Capability advertisement

The host sets bit `0x40000000` in the `x-ss-general.featureFlags` attribute of its RTSP DESCRIBE
response when clipboard sync is enabled on the host, enabled for the client's certificate, and a
clipboard backend works on the host. Clients read it with `LiGetHostFeatureFlags()`. Formats, files,
limits and directions are negotiated in HELLO.

## Upgrade

The client opens TLS to the host's HTTPS port with its paired client certificate and sends, with no
body:

```
GET /clipboard HTTP/1.1
Host: <address>:<port>
Connection: Upgrade
Upgrade: sunshine-clipboard
Sunshine-Clipboard-Version: 1

```

Success is `HTTP/1.1 101 Switching Protocols` with `Connection: Upgrade`,
`Upgrade: sunshine-clipboard` and `Sunshine-Clipboard-Version: 1`. Failures close the connection after
the response:

| Status | Meaning |
|--------|---------|
| `200` with an XML body whose `status_code` is `401` | The client is not paired or is disabled. The HTTPS server's client check refuses it before any request is read, the GameStream way |
| 403 | Clipboard sync disabled globally or for this client, or a new certificate beyond the 8-channel total |
| 409 | No running stream for this certificate (the client may retry with backoff) |
| 400 | Bad request: a body (`Content-Length` other than 0, or any `Transfer-Encoding`), or missing headers |
| 426 | Unsupported `Sunshine-Clipboard-Version` |
| 404 | Wrong path |

A client that gets `200` to its upgrade request reads the body, bounded by its `Content-Length`, and
takes the XML `status_code` as the refusal status.

The client sends nothing after the request until it has read the `101` header. Bytes after the
header's blank line are frames.

The host admits one channel per certificate: a new upgrade from the same certificate replaces the
old channel. At most 8 channels are open in total.

## Framing

Each frame is a 12-byte header followed by the payload. Integers are little-endian.

| Field | Type | Meaning |
|-------|------|---------|
| `length` | u32 | 8 + payload size |
| `type` | u16 | Message type (see below) |
| `flags` | u16 | Bit 0 is END; other bits are ignored |
| `stream_id` | u32 | 0 for the channel itself, otherwise the request's stream |

The payload is at most 65536 bytes for every type.

Stream ids: the requester picks a new id for each request. **Client-initiated ids are odd,
host-initiated ids are even**, and ids are never reused within a channel.

**Flags.** END is set on the last DATA frame of a response, on LOCK_REPLY, and on an ERROR that ends a
stream. It is the only way a response ends. Requests and every other type are sent with flags 0, and
receivers ignore flags on them.

**Numbers.** Every size, offset, length, count, credit and limit (`size`, `total_bytes`, `count`,
`bytes`, `offset`, `length`, `max_item_size`, `max_transfer_size`) is a byte or item count as a
non-negative JSON integer (at most 2^53−1). `generation`, `lease`, `index` and `nonce` are
non-negative JSON integers. `mtime` is integer unix seconds. A float, string or negative value where
an integer is required is `bad_request`.

## Messages

Payloads are UTF-8 JSON unless noted. Unknown message types and unknown fields are ignored.

| # | Name | stream_id | Payload |
|---|------|-----------|---------|
| 1 | HELLO | 0 | `{"version":1,"os":"macos","formats":[mime...],"policy":{"host_to_client":b,"client_to_host":b,"files":b},"limits":{"max_item_size":n,"max_transfer_size":n}}` |
| 2 | HELLO_REPLY | 0 | `{"version":1,"os":"linux","formats":[mime...],"policy":{...effective...},"limits":{...effective...},"host_clipboard":b}` |
| 3 | ANNOUNCE | 0 | `{"generation":n,"formats":[{"mime":"text/plain;charset=utf-8","size":n},{"mime":"files"}]}` (`size` optional) |
| 4 | ANNOUNCE_ACK | 0 | `{"generation":n,"status":"installed"}` or an error code as `status` |
| 5 | CLEAR | 0 | `{"generation":n}`: the sender's latest generation, 0 if it never announced |
| 6 | GET | new | `{"generation":n,"mime":"..."}` → DATA … END |
| 7 | LOCK | new | `{"generation":n}` → LOCK_REPLY |
| 8 | LOCK_REPLY | same | `{"lease":n,"count":n,"total_bytes":n}`, flag END |
| 9 | UNLOCK | 0 | `{"lease":n}` |
| 10 | GET_FILE_LIST | new | `{"lease":n}` → DATA … END carrying NDJSON lines |
| 11 | GET_FILE | new | `{"lease":n,"index":n,"offset":n,"length":n}` → DATA … END carrying exactly `length` bytes |
| 12 | DATA | same | Raw bytes. An empty payload with END is a valid empty result |
| 13 | CREDIT | same | `{"bytes":n}` grants the responder n more DATA payload bytes on that stream |
| 14 | CANCEL | same | `{}`: the requester aborts; the responder stops and late frames for the stream are discarded |
| 15 | ERROR | 0 or same | `{"code":"...","message":"..."}`; on stream 0 it is fatal and the sender closes the channel |
| 16 | PING | 0 | `{"nonce":n}` |
| 17 | PONG | 0 | `{"nonce":n}`, echoing the PING's nonce |

GET_FILE_LIST entries are NDJSON lines, each ending in `\n`; a line may span DATA frames:

```
{"index":n,"path":"a/b","size":n,"mtime":n,"dir":b}
```

`path` is `/`-separated relative UTF-8. Directories come before their contents and have `size` 0.

## Handshake

1. After the `101`, the client sends HELLO. HELLO must arrive within 10 s.
2. `policy` in HELLO is what the client allows; a policy field missing from HELLO means false.
   HELLO `limits` are the client's wishes in bytes; a missing limit means the host's value.
3. The host answers HELLO_REPLY. Its `policy` is the intersection of the client's policy with the
   host's global setting and the client's per-client permission. Each of its `limits` is the minimum
   of the client's value, the host's configured value and the host's hard cap. Its `formats` is the
   intersection of the client's formats with the host's.
4. When `host_clipboard` is true, the host's ANNOUNCE of its current clipboard is the next frame after
   HELLO_REPLY. The client does not announce on connect.

Both sides enforce the effective limits on actual bytes.

## Formats

Canonical formats:

| Mime | Content |
|------|---------|
| `text/plain;charset=utf-8` | UTF-8 text with LF line endings on the wire |
| `text/html` | UTF-8 HTML |
| `text/rtf` | RTF, passed through |
| `image/png` | PNG image |
| `files` | Files and folders, fetched with LOCK, GET_FILE_LIST and GET_FILE |

HELLO `formats` lists the canonical formats the client can both send and receive. A sender announces
only formats in the effective intersection, and `files` only when the effective `policy.files` is
true. A GET for a format the generation did not announce is answered `unsupported_format`. An
ANNOUNCE with an empty `formats` list is valid: the sender's clipboard holds nothing transferable,
and the receiver installs an empty clipboard that it owns.

Sizes in ANNOUNCE are hints and usually absent; the Linux host never sends them. Receivers enforce
every limit on the actual bytes they pull.

## Generations and acknowledgements

Generations are per sender per channel, start at 1 and strictly increase. Sending ANNOUNCE makes the
sender the owner of the shared clipboard for that generation; the newest ANNOUNCE wins.

The receiver of an ANNOUNCE sends exactly one ANNOUNCE_ACK for it: after it has installed native
clipboard ownership for exactly that generation or failed to (never before), or immediately when it
rejects it outright (a well-formed ANNOUNCE with more than 32 formats or a mime longer than 128 bytes
is `bad_request`; a direction the effective policy forbids is `not_permitted`). A receiver may
legitimately never answer an announcement it neither tries to install nor rejects, for example an
unfocused client or a client that does not download files automatically.

The sender never requires an ACK and never re-announces or releases ownership because one is missing.
It ignores an ACK for a generation it never announced or has already resolved. An ACK for any
generation the sender still tracks as pending is delivered to its consumer even when newer
generations have been announced since.

| ACK status | Meaning |
|------------|---------|
| `installed` | Native ownership was taken for exactly that generation |
| `stale_generation` | Superseded by the sender's newer ANNOUNCE or CLEAR before installation finished, or a GET or LOCK for it was answered `stale_generation` |
| `not_permitted` | Policy, or the receiver's OS denied access |
| `too_large` | The payload or transfer exceeds the effective limits, the receiver's local caps or its free disk space |
| `bad_request` | Rejected outright (see above) |
| `io_error` | Anything else failed, including staging and short file data |

CLEAR means the sender's clipboard was emptied, released, or is not available to this peer. CLEAR
naming an unknown or older generation, and UNLOCK naming an unknown lease, are ignored.

GET and LOCK answer `stale_generation` unless they name the sender's current generation.

## Errors

Channel-fatal errors are sent as ERROR on stream 0, after which the sender closes the channel:

| Condition | Code |
|-----------|------|
| Frame `length` < 8 or payload > 65536, or DATA beyond granted credit | `limit_exceeded` |
| Malformed JSON or a missing or mistyped required field in a stream-0 message, any frame before HELLO, a second HELLO, HELLO `version` other than 1, a request reusing a live or past stream id or using the other side's parity | `bad_request` |

Per-stream errors are sent as ERROR on that stream with END; the channel stays open:

| Condition | Code |
|-----------|------|
| Unknown generation or not the sender's current one | `stale_generation` |
| Unknown, expired or released lease, or index out of range | `unknown_lease` |
| Format not announced, or LOCK on a generation that did not announce `files` | `unsupported_format` |
| Direction or files not allowed by the effective policy, or OS access denied | `not_permitted` |
| A payload exceeding `max_item_size`, or a manifest or total exceeding `max_transfer_size` or the hard caps | `too_large` |
| `offset + length > size`, or malformed request JSON | `bad_request` |
| More than 8 concurrent streams from the peer, or more than 4 leases | `busy` |
| Read failure, premature EOF, identity mismatch, no progress for 30 s | `io_error` |

A receiver that sees a response exceed a declared or negotiated size sends CANCEL and treats the
stream as failed with `too_large`; one that sees a GET_FILE response whose total differs from the
requested `length` sends CANCEL and treats it as failed with `io_error`. A receiver verifies the
manifest's entry count and summed file sizes against LOCK_REPLY's `count` and `total_bytes` before
using it.

## Flow control

Each response stream starts with 1048576 bytes of DATA credit. The receiver grants more with CREDIT as
it consumes data; the sender never sends beyond granted credit, and DATA beyond credit is a fatal
`limit_exceeded`. The receiver always drains the socket. Control frames are written ahead of queued
DATA, so a slow consumer never stalls PING or other streams. A sender's write queue holds at most
2 MiB before its producers pause.

## Leases and files

LOCK builds and pins the file manifest of a generation, independently of later clipboard changes,
and answers a lease. Leases are per lease owner per channel and start at 1. `count` and
`total_bytes` in LOCK_REPLY are authoritative. A lease ends on UNLOCK, after 10 minutes without a
request on it, or when the channel closes (in-flight transfers then fail with `io_error`). At most 4
leases exist per channel. GET_FILE only serves entries of the lease's own manifest, and
`offset + length` must not exceed the entry's `size`.

## Timers

- PING every 10 s from each side. 30 s without any received frame closes the channel.
- A stream with no progress for 30 s is cancelled with `io_error`.
- A lease expires after 10 minutes without a request on it.
- HELLO must arrive within 10 s of the `101`.

## Source revision fence

Each sent generation is bound to the native clipboard revision it announced (macOS `changeCount`,
the Wayland offer object, the Windows clipboard sequence number). Serving GET or LOCK for generation N checks before reading that the native
clipboard is still at N's revision and holds no sensitive-marked content, and checks again after
reading. On a mismatch the result is discarded and the request is answered `stale_generation`.

## Installation guards

A receiver commits a completed pull to its native clipboard only if, at commit time, the channel is
still open and policy still allows the direction, the pulled generation is still the sender's current
one (no newer ANNOUNCE or CLEAR arrived), and its local clipboard has not changed since the pull
began (a local copy made meanwhile wins and is announced instead). CLEAR, a newer ANNOUNCE, a policy
change and channel close cancel pending pulls and installations.

An authorized peer can GET announced data without a paste gesture on the other side. Privacy rests on
trust in the paired, currently streaming peer, on the effective policy (host global and per-client
settings intersected with the client's per-host settings, each one switch to turn off), and on
sensitive-content filtering on both ends.

## Host ownership rules

- The host keeps one owner of its clipboard: the host itself or one channel.
- When the host clipboard changes and Sunshine is not the owner, the host becomes the owner and sends
  ANNOUNCE to every channel whose policy allows host to client. When the new content is not
  transferable to a channel (the clipboard is empty, holds no format in the channel's intersection,
  or holds sensitive content), the channel gets CLEAR if it still holds the host's previous
  generation, and nothing otherwise.
- When a channel's ANNOUNCE is installed, that channel becomes the owner and every other channel gets
  CLEAR. Other channels can never read that client's data.
- Last writer wins. When the owning channel closes, or sends CLEAR for its clipboard, the host empties
  its clipboard if Sunshine still owns it.

## Hard limits

These local caps apply whatever the peer requests:

- Channels: 1 per certificate, 8 in total.
- Frames: payload at most 65536 bytes for JSON and DATA.
- Streams: at most 8 concurrent per direction (excess is answered `busy`).
- ANNOUNCE: at most 32 formats, mime at most 128 bytes; a non-file item at most the effective
  `max_item_size`.
- Images: decoded at most 16384×16384 and 256 MiB before conversion.
- Files: manifest at most 100000 entries, depth at most 64, path segment at most 255 UTF-8 bytes,
  path at most 4096 bytes, manifest at most 32 MiB, total at most the effective `max_transfer_size`
  (itself capped at 1 TiB); at most 4 leases per channel; staging needs the total in free disk space
  before it starts.

## File containment

Both sides apply these rules.

- Received paths are relative only: no `..`, empty or `.` segments; no NUL or control characters; no
  backslash, drive letters, UNC prefixes or `:`; no Windows reserved names or trailing dots or spaces.
  Names that collide after case folding and Unicode NFC normalisation, or with an existing entry at
  the destination, are renamed with a ` (n)` suffix. Directory entries precede their contents.
- Staging roots are private: a random directory created with owner-only permissions under a per-user
  location, verified to be a real directory and not a link before use. Files are created relative to
  the opened root (`openat` with `O_NOFOLLOW` on POSIX), never by re-resolving path strings, and
  cleanup walks by handle and never follows links.
- The sending side builds the manifest by walking from opened handles. Symlinks are not followed (they
  are sent as nothing) and special files are skipped. GET_FILE opens the entry relative to its parent
  directory and checks that the file's identity (device and inode) matches the one recorded in the
  manifest, so a path replaced after enumeration is refused.

## Linux host

The Linux host uses the `ext-data-control-v1` Wayland protocol (KDE Plasma 6 and other compositors
that implement it). Compositors without it, such as GNOME's, give a background service the clipboard
only through the XDG desktop portal, so there the host opens a portal remote desktop session with
clipboard access. The session also asks for the keyboard, which the host never uses, because GNOME
remembers the permission only for a session with an input device. The desktop asks the user to
allow the session when Sunshine starts;
when the user lets it remember the choice, the portal's restore token, kept in
`clipboard_portal_token` in the config directory, lets later starts skip the question. Until access is
allowed, the host announces nothing and refuses clients' content.

Native to canonical, in preference order:

| Native types | Canonical |
|--------------|-----------|
| `text/plain;charset=utf-8`, `UTF8_STRING`, `text/plain` | `text/plain;charset=utf-8` (CRLF and lone CR become LF; invalid UTF-8 is replaced) |
| `text/html` | `text/html` (UTF-16 with a BOM is converted to UTF-8) |
| `text/rtf`, `application/rtf` | `text/rtf` |
| `image/png` | `image/png` |
| `text/uri-list` with at least one `file://` URI | `files` (only `file://` entries are used) |

Selections offering `x-kde-passwordManagerHint` are never announced.

When a client owns the host clipboard, the host offers `text/plain;charset=utf-8`, `text/plain` and
`UTF8_STRING` for text, `text/html`, `text/rtf` and `application/rtf`, `image/png`, and always
`x-kde-passwordManagerHint` with the bytes `secret`, so clipboard managers neither record a client's
content nor restore it after the client's channel closes.

Files from a client are staged before they are offered, because file managers copy from the offered
paths at once and Qt applications read the clipboard synchronously. On an ANNOUNCE with `files`, the
host LOCKs, fetches the manifest and every file into a private staging root under
`$XDG_CACHE_HOME/sunshine/clipboard/`, verifies the count and total against LOCK_REPLY, and only then
takes ownership and sends ANNOUNCE_ACK. The staged selection offers, with one `<uri>` per top-level
manifest entry in manifest order (`file://` and the absolute staged path, every byte outside
`A-Z a-z 0-9 - . _ ~ /` percent-encoded as uppercase `%XX`):

- `text/uri-list`: each `<uri>` followed by `\r\n`;
- `x-special/gnome-copied-files`: `copy`, then `\n<uri>` per entry, with no trailing newline;
- `text/plain;charset=utf-8` and its aliases: the absolute staged paths, unencoded, joined with
  `\n`, unless the client announced its own text format, which then takes precedence.

A staging root is released when its generation is superseded or cleared or its channel closes. A
released root is deleted one hour after release and never earlier; the release time is kept in a
marker file `<root>.released` beside the root. A reaper runs every 10 minutes and before every
free-space check. When a new staging does not fit in the free space, the transfer is refused with
`too_large`. At service start, a root without a marker is adopted as released at that moment. Only
directories in the staging base are roots; other files there are never adopted or deleted as roots.
The root itself is owner-only; entries inside it get the user's normal permissions (umask applied), so
a native paste that copies them does not leave owner-only files at the destination.

## Windows host

The Windows host uses the Win32 clipboard, which belongs to the window station. SunshineService runs
Sunshine as SYSTEM in `WinSta0` of the console session, the window station of the logged-on user's
applications, so Sunshine shares their clipboard whichever desktop has input.

Windows shows a process running as SYSTEM only the predefined text and bitmap formats of the user's
content; HTML, RTF, PNG, file lists and other registered formats are hidden from it. Sunshine
therefore starts `sunshine.exe --clipboard-agent` as the logged-on user in the console session and
inspects and reads the clipboard through it, over pipes on the agent's standard input and output.
Sunshine itself watches for changes and puts client content on the clipboard. While nobody is logged
on, or the agent cannot run, Sunshine reads what it can see itself.

Native to canonical, in preference order:

| Native formats | Canonical |
|----------------|-----------|
| `CF_UNICODETEXT`, also synthesized by Windows from `CF_TEXT` | `text/plain;charset=utf-8` (up to the first NUL; CRLF and lone CR become LF) |
| `HTML Format` | `text/html`: the document between `StartHTML` and `EndHTML`, or the fragment when the document range is missing |
| `Rich Text Format` | `text/rtf` (up to the first NUL) |
| `PNG` or `image/png`; otherwise the first of `CF_DIBV5` and `CF_DIB` | `image/png` (bitmaps are converted; an alpha channel that is entirely zero counts as opaque) |
| `CF_HDROP` | `files` |

Content that carries `ExcludeClipboardContentFromMonitorProcessing` or `Clipboard Viewer Ignore`, or
sets `CanIncludeInClipboardHistory` or `CanUploadToCloudClipboard` to 0, is never announced.

When a client owns the host clipboard, the host puts every format on the clipboard at once rather
than on request: `CF_UNICODETEXT` with CRLF line endings, `HTML Format`, `Rich Text Format`, `PNG`
together with `CF_DIBV5` (straight alpha) and `CF_DIB` (24-bit, composited on white), and for staged
files `CF_HDROP` and `Preferred DropEffect` set to copy, with the text formats carrying the staged
paths unless the client announced its own text. It always adds
`ExcludeClipboardContentFromMonitorProcessing`, and `CanIncludeInClipboardHistory` and
`CanUploadToCloudClipboard` set to 0, so neither the clipboard history nor the cloud clipboard keeps
a client's content.

Files from a client are staged as on Linux, into `%LOCALAPPDATA%\Sunshine\clipboard\` of the user
logged on to Sunshine's session. Running as SYSTEM, Sunshine acts as that user for all clipboard file
access, so host files are read with that user's rights and drive mappings, and staged files belong to
that user. The staging area and every root get a protected access list that grants full control to
that user and SYSTEM only; entries inside inherit it. Characters Windows file names cannot hold
(`< > " | ? *`) become `_` before collision renaming. Links are name-surrogate reparse points
(symbolic links, junctions and mount points); other reparse points, such as cloud-file placeholders,
are ordinary files. The identity check compares the volume serial number and the file id.

## Client contract

These rules are client behaviour; the host relies on them but does not check them.

- The client opens the channel only after its stream has started, when the host's feature bit is set
  and the user has not disabled clipboard sync for this host. It retries `409` a few times with
  backoff.
- A refusal (any `4xx`, or a `200` refusal carrying an XML `status_code`) is never retried. The client
  treats `401` exactly like `403` and shows one generic notice that the host refused clipboard sync
  for this device, since `403` has more than one cause.
- After a connection-level failure (a TCP or TLS error, the idle timeout, or the host closing the
  channel) while the stream runs, the client reconnects up to 5 times, 5 s apart. The new channel
  replaces the old one on the host and starts its generations and leases afresh.
- The client sends only the directions and features the user allows for this host in HELLO, and
  leaves `client_to_host` out when the OS denies it access to its clipboard.
- The client announces its clipboard on later local changes, when its stream window regains focus,
  and at the paste barrier, never merely on connect.
- Content the client's OS marks as sensitive or transient is not announced.
- When its clipboard holds file URLs, the macOS client announces only `files`; the host's text
  aliases then carry the staged paths. When it holds nothing transferable, it announces an empty
  `formats` list.
- Manifest paths are sent in Unicode NFC; receivers still apply the collision rules.

### Paste barrier

The paste barrier keeps a paste in the stream from reaching the host before the host holds the
client's current clipboard. The client tracks:

- `synced_rev`: the latest local clipboard revision the host is known to hold or to have superseded
  — the revision of an ANNOUNCE whose ACK came back `installed`, or the revision produced by the
  client's own installation of host content. Sending an ANNOUNCE does not move it. At channel open it
  is the current revision when HELLO_REPLY's `host_clipboard` is true (the host's clipboard wins at
  connect), and "nothing synced" otherwise.
- `pending`: each sent, unacknowledged generation and the revision it announced.

| State | Event | Action |
|-------|-------|--------|
| Channel not open, or `client_to_host` not in the effective policy | Paste key | Pass through; the host pastes its own clipboard |
| Idle | Paste key, current revision R equals `synced_rev` | Pass through |
| Idle | Paste key, R differs from `synced_rev` | Hold the paste and every later key, mouse-button and wheel event in order (motion, touch and controller events pass); start a 2 s deadline; wait for the ACK of the pending generation that covers R, or send a new ANNOUNCE for R and wait for its ACK |
| Holding | Another paste key | Join the queue; the deadline is not extended |
| Holding | `installed` ACK for the generation covering R | Set `synced_rev` to R and release the queue in order |
| Holding | Error ACK for that generation, 2 s deadline, focus loss, or channel close | Drop the held paste keys (and their key-ups), show a notice, release the rest |
| Holding | ACK for another generation | Ignored by the barrier |
| Any | CLEAR from the host for its current generation | Set `synced_rev` to "nothing synced", forget the host's unpulled generation, cancel pending pulls |

A paste of files may take longer than the 2 s deadline while the host stages them; the client then
drops the held paste, shows the transfer's progress, and tells the user to paste again once the
`installed` ACK arrives. A later paste whose revision is covered by the same pending ANNOUNCE waits
for that same ACK.

Host to client is eventually consistent: until a pull completes, a paste on the client gets its
previous clipboard.
