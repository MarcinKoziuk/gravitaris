# ADR 0003: IAudioBackend abstraction over OpenAL and miniaudio

Status: accepted, deliberately transitional (see "When to remove this" below)

## Decision

Gameplay code (`AudioSystem`) never touches `Magnum::Audio` or `miniaudio`
directly. It goes through `IAudioBackend`, an interface of opaque handles
(`SoundBufferHandle`, `VoiceHandle`) and verbs (`UploadBuffer`, `AcquireVoice`,
`PlayOneShot`, `PlayLooping`, ...). Two implementations exist:

- `MagnumOpenALBackend` — the original OpenAL Soft (via Magnum::Audio) code.
- `MiniaudioBackend` — miniaudio (single vendored header, `extlibs/miniaudio/`).

`CreateAudioBackend(AudioBackendPreference)` constructs exactly one backend,
no fallback: `Auto` always resolves to miniaudio (see "Final diagnosis"
below -- OpenAL is unusable as an automatic default on *any* platform in this
project's current link topology) and if that backend's `Init()` fails, audio
disables outright rather than silently substituting a different backend the
caller didn't ask for. `PreferOpenAL`/`PreferMiniaudio` force one explicitly,
same no-fallback rule; OpenAL stays reachable this way for testing/comparison
via the debug UI's "Audio" tab (`AudioSystem::SetBackendPreference`), which
tears down the current backend and rebuilds it, re-uploading every still-live
`AudioClip`.

## Why

Positional audio (bullets/hits/thruster) crashed/produced no sound on macOS.
Initial diagnosis (later found incomplete, see "Update" below): `Magnum::
Audio::Context::tryCreate()` calls `alcMakeContextCurrent()` but never checks
its return value. On that machine's CoreAudio setup, `tryCreate()` reports
success (Magnum's own bookkeeping believes the context is current) while
`alcGetCurrentContext()` reads back `nullptr` -- every AL call afterward then
fails with `AL_INVALID_OPERATION`.

**Update (a following session, reproduced on Windows):** the symptom is not
macOS/CoreAudio-specific, and the actual root cause is different from the
initial diagnosis above. Direct instrumentation showed `tryCreate()` returns
`true` and even calls `alcMakeContextCurrent()` with a genuinely valid
context, yet `alcGetCurrentContext()` -- called from `MagnumOpenALBackend::
Init()`, compiled into `GravitarisNG.exe` -- reads back `nullptr`
immediately after. A hand-rolled ALC sequence (open/create/make-current)
issued from that same `Init()` call *did* report success. That session
concluded `Context::tryCreate()` itself was broken and reworked
`MagnumOpenALBackend` to own the `ALCdevice`/`ALCcontext` directly instead of
going through `Magnum::Audio::Context` -- which "fixed" the false-negative
`Init()` check, but playback was still silent, because the actual bug is one
level further down.

## Final diagnosis: OpenAL Soft is static, and static state doesn't cross a DLL boundary

`Magnum::Audio` is built as `MagnumAudio-d.dll` (a shared library); OpenAL
Soft is statically linked, per this project's own choice (see "Decision" in
the OpenAL section of `CMakeLists.txt`) to dodge the `OpenAL32.dll`-name
collision with a legacy Creative router present in `System32` on some
machines (this one included). Static-linking a library into two *separate*
binaries gives each binary its own private copy of that library's global
state. OpenAL Soft's ALC layer keeps exactly that kind of global state (which
context is "current"). So:

- `MagnumAudio-d.dll` has its own copy of OpenAL Soft, and it's this copy
  that every real `Audio::Buffer`/`Audio::Source`/`Audio::Renderer` call
  executes against (they're implemented inside the DLL).
- `GravitarisNG.exe` -- once `MagnumOpenALBackend::Init()` started calling
  `alc*` functions directly (the previous session's rework) -- has its
  *own, separate* copy, and that's the one `alcOpenDevice`/`alcCreateContext`/
  `alcMakeContextCurrent`/`alcGetCurrentContext` in `Init()` were operating
  on.

Both copies "worked" in isolation -- that's exactly why the hand-rolled ALC
sequence in `Init()` reported success. But a context made current in the
exe's copy is invisible to the DLL's copy, and vice versa: whichever module
actually made a context current, the *other* module's AL calls see no
context at all and every one of them silently fails with
`AL_INVALID_OPERATION`. This is precisely the failure class "Why miniaudio"
below already named ("two independent link units disagree about backend
state") -- it just wasn't yet understood to also explain *this* symptom when
that section was written.

This also retroactively explains why the original, pre-refactor code (a
single `AudioSystem` that never called raw `alc*` functions itself, only
`Magnum::Audio::Context::tryCreate()`) worked: with no direct ALC calls from
the exe, only the DLL's copy of OpenAL Soft was ever touched, so there was
only one copy of the global state in play, and it was self-consistent.

**Consequence**: `MagnumOpenALBackend` cannot be made reliable through any
amount of ALC-sequence fixing in `Init()` -- the bug is the split itself, not
which sequence of calls populates either copy. Fixing it for real means
either (a) never calling `alc*` directly from the exe (revert to trusting
`Magnum::Audio::Context::tryCreate()`, accepting its own unresolved
false-success quirk from the original diagnosis above), (b) building Magnum
statically so there's only one binary and thus one copy of OpenAL Soft's
state, or (c) not defaulting to OpenAL at all. This session took (c): `Auto`
now always resolves to miniaudio (single link unit, all calls originate from
the exe, no split possible), on every platform, not just `__APPLE__`.
`MagnumOpenALBackend` is left in place, reachable via the debug UI, for
whoever eventually pursues (a) or (b) -- see "When to remove this".

## Why miniaudio as the default (not just "disable audio")

Single-file, public-domain, no build-system surface (drop-in header +
`MINIAUDIO_IMPLEMENTATION` translation unit, no FetchContent, no separate
dylib) -- which directly rules out the "two independent link units disagree
about backend state" class of bug this session confirmed is the actual root
cause (see "Final diagnosis" above). Native backends cover macOS, iOS,
Windows, Linux, Android in one dependency. See the session's chat log for the
fuller backend comparison (SoLoud, FMOD, SDL2 audio) that led here; miniaudio
won on "zero packaging surface" + broad platform coverage + no license to
track.

No silent fallback: trying OpenAL first and falling back to miniaudio on
failure meant paying a failed-`Init()` cost on every launch for a backend
known not to work as an automatic default, and it made "what's actually
running" one step removed from what was asked for. `Auto` resolves once, up
front, to miniaudio; if that fails, audio disables rather than quietly
landing on a different backend.

## API-shape differences that mattered for the implementation

OpenAL's `Source` is buffer-agnostic (`setBuffer()` rebinds anytime).
miniaudio's `ma_sound` is bound to exactly one data source at
`ma_sound_init_from_data_source()` time -- there is no rebind call.
`MiniaudioBackend::PlayOneShot`/`PlayLooping` re-init the underlying
`ma_sound` on every call to keep `VoiceHandle`'s cross-backend contract
("assign any buffer, anytime") identical; cheap at our call rate (a handful
of plays/second).

**`ma_audio_buffer` owns a single playback cursor** (its own docs: "An audio
buffer has a playback cursor just like a decoder"). The first implementation
uploaded one `ma_audio_buffer` per `AudioClip` and bound every voice playing
that clip to the same buffer object -- so concurrent instances of one clip
shared one cursor. Symptom: firing repeatedly, only the first bullet was
audible (later shots' voices read from wherever the first shot's cursor had
already advanced to); two or more ships thrusting simultaneously produced
torn/distorted audio (their loop-voices raced the same cursor every mix
callback). Fix: `BufferSlot` now owns the decoded PCM directly (a
`std::vector<int16_t>`, not an `ma_audio_buffer`); each *voice* gets its own
`ma_audio_buffer_ref` -- a lightweight, non-owning data source with an
independent cursor -- pointing at that shared read-only memory, created fresh
(`ma_audio_buffer_ref_init` unconditionally zeroes the struct, cursor
included) on every `RebindVoice` call. N concurrent voices playing the same
clip now advance independently. Verified via distinct-address logging across
repeated one-shot fires (8 pool voices round-robining, fresh zeroed cursor
each time) and 3 simultaneous thruster loops on the same clip.

Known residual edge case, not yet handled: if a `BufferSlot` is released and
its index reused for a *different* clip's upload while some old voice still
holds an `ma_audio_buffer_ref` into the original data, that ref would dangle.
Not currently reachable in practice -- `AudioClip`s are loaded once at startup
and never reloaded mid-game, and a backend switch releases every voice before
any buffer -- but worth a generation-counter guard (same pattern as
`PhysicsRef`, `docs/adr/0002-physics-ownership.md`) if clips ever become
hot-reloadable.

`ma_sound`/`ma_audio_buffer_ref` (and OpenAL's `Buffer`/`Source`) are
referenced by raw pointer once initialized (miniaudio's/OpenAL's own internal
bookkeeping), so -- same lesson as `docs/adr/0002-physics-ownership.md` --
each lives in an individually heap-allocated slot (`std::unique_ptr`) inside a
growable vector, never directly as a vector element that could be relocated
by growth.

## AudioClip: reactive loading via ResourceLoader

Sound assets are `AudioClip` resources (`IResource`, decoded PCM16, mono),
loaded through `ResourceLoader` and uploaded reactively via
`OnCreate<AudioClip>`/`OnDestroy<AudioClip>` observers -- mirroring
`ModelRenderer2`'s `OnCreate<Model>`/`OnDestroy<Model>` pattern -- rather than
the previous ad hoc `IFilesystem::ReadBytes` + inline WAV parse. A failed
load silently substitutes `AudioClip::Placeholder()` (an empty/silent clip),
same graceful-degrade convention as `Model`/`Body`.

Music is explicitly out of scope for `AudioClip`/`IAudioBackend` as they
stand: a multi-minute track needs streaming decode (compressed source,
decoded a chunk at a time) rather than a fully-resident `ResourceLoader`
resource, and isn't positional. That's a separate abstraction for when music
assets actually exist (see IDEAS.md) -- likely a few additional
`IAudioBackend` methods (`OpenMusicStream`/`PlayMusic`/...) rather than a
second parallel system.

## When to remove this

This abstraction is scaffolding, not a permanent two-backend commitment.
`MagnumOpenALBackend` cannot become a reliable default without first fixing
the underlying split (see "Final diagnosis"): either building Magnum
statically (one binary, one copy of OpenAL Soft's state) or reverting
`Init()` to never call `alc*` directly (trusting `Context::tryCreate()`,
which has its own unresolved false-success quirk from the original
diagnosis). Absent that work, once miniaudio is confirmed solid across every
target platform, just delete `MagnumOpenALBackend`, `AudioBackendPreference`,
`CreateAudioBackend`, and the debug UI's backend picker; `AudioSystem` keeps
just `IAudioBackend` and `MiniaudioBackend`.
