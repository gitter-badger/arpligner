<noscript><a href="https://liberapay.com/Ywen/donate"><img alt="Donate using Liberapay" src="https://liberapay.com/assets/widgets/donate.svg"></a></noscript> <img src="https://img.shields.io/liberapay/patrons/Ywen.svg?logo=liberapay">

# Arpligner

Arpligner is a **multi-track** & **polyphonic** arpeggiator which will use your
own arpeggiation patterns (like Xfer Cthulhu, Reason PolyStep Sequencer, FL VFX
Sequencer, 2Rule TugMidiSeq). It is packaged as a VST3 MIDI plugin and as a
standalone application.

- **Multi-track**: Multiple arpeggiation patterns can play at the same time for
  a given chord, each one in its own track of your DAW, or on its own MIDI
  channel,
- **Polyphonic**: Each step can play several notes of a chord at the same
  time. "Steps" can also hold over an arbitrary length of time and overlap.  So
  besides arpeggiation _per se_, you can do strumming or really any kind of
  "turning a plain block chord into something more interesting".

To achieve this, the big difference between Arpligner and the aforementioned
plugins is that Arpligner does very intentionally _**not** come with its own
graphical interface for editing patterns_. Instead it will rely on arp patterns
being fed to it as regular (and possibly live) MIDI data: you can thus
make use of your DAW piano rool and MIDI sequencing capabilities[^1], play those
patterns live, or use an external MIDI sequencer (software or hardware).

Therefore, you can use it as a regular arp, playing your chords against
pre-written patterns, or the opposite. Or both can be live data! That would be
having two keyboard players: one in charge of the chords and the other one in
charge of how to layout those chords[^2].

See https://youtu.be/IQ9GFEaS4Ag for an overview of the tool and the basic
features (this uses the original Lua script but the video remains valid).

Still experimental, please post issues here in case of bugs or questions! :)

## How to use it

Arpligner has two modes: **Multi-channel** and **Multi-instance**, which affect
how Arpligner will receive MIDI data and set what it considers to be a
"**chord** track" and what it considers to be a "**pattern** track".

### General behaviour

Whatever the mode, Arpligner will interpret "notes" playing on **pattern**
tracks as the degrees of the chord which is currently playing on the **chord**
track. Starting from C3 and going up:

- C3 means "first chord degree"
- C#3 means "second chord degree"
- D3 means "third chord degree", etc.

C3 is therefore treated as the _reference_ pattern note, the one from which
everything will be derived.

However, you may wonder then what happens if you go below C3. Or if you go
"above" the last degree of your chord. Well then, Arpligner will loop through
the chord, but transposing the notes up or down as many octaves as needed, so it
always has a sensible chord note to play.

You can also select a different mapping method. For instance, you can choose
that only the white keys will be used here, which can be more convenient when
playing patterns live on a MIDI keyboard. See the [pattern settings
section](#pattern-parameters) for more info.

### Multi-channel mode

In this mode, you can use as little as one single Arpligner instance in your DAW
session. It expects to be fed MIDI data on at least 2 channels:

- Channel 16 (by default), which will be treated as the **chord** channel
  ("track")
- At least one _other_ channel (say, Channel 1), which will be treated as a
  **pattern** channel ("track"). Any channel other than the **chord** channel
  will do, they will all be treated equally.

You can perfectly use several Arpligner instances set to that mode at the same
time, but they will just be completely independent of one another. They will
each receive their own chord track and pattern tracks.

**Multi-channel** has the advantage of requiring a bit less CPU and RAM, and
enables you to have several chord tracks, each one affecting up to 15 patterns
tracks.

### Multi-instance mode

In this mode, the different instances of the Arpligner plugin in your DAW
session can communicate with one another. You would then have one Arpligner
instance per relevant track, one being configured as the **Global chord instance**,
and the other ones as **Pattern instances**. MIDI channels no longer matter to
Arpligner in that mode.

**Multi-instance** has the advantage of alleviating MIDI routing configuration
in your DAW, and is not limited by the number of MIDI channels. So you have just
one chord track, but it can affect any number of pattern tracks.

Note that it is perfectly possible to have in your DAW session both
**Multi-instance** instances and **Multi-channel** instances. Only those set to
**Multi-instance** will communicate, the other ones will keep depending solely
on the MIDI data you directly feed into them.


## Installation

Please go to the [releases](https://github.com/YPares/arpligner/releases)
(unfold the "Assets" section) to download the latest release. Alternatively,
this repository comes with all the needed code to build the plugin and
standalone application.

Then install the VST3 plugin in your regular VST3 folder, depending on your system
and DAW settings.

**Important:** Due to [a VST3
limitation](https://forum.juce.com/t/arpeggiatorplugin-vst3-recognized-as-aufio-fx-instead-of-midi-fx/43563),
Arpligner will be recognized by your DAW as an Audio Fx plugin, whereas it is
only a MIDI Fx plugin. So if your DAW sorts or filters plugins by categories,
you will have to look for Arpligner under this category.

Then, I recommend to have one track dedicated to **chords**. After that, if you
use the **Multi-channel** mode, you'll need some way to configure MIDI routing
in your DAW so Arpligner receives what it expects. See [this
section](#tips-for-multi-channel-mode) for tips.

## Implementation & supported plugin formats

Arpligner is implemented in C++ with [JUCE 7](https://juce.com/), and therefore
should support a variety of systems and plugin formats. For now, I'm providing
VST3 builds for Linux and Windows x64, but you can have a look under the
`Builds` folder for supported platforms. The [project file](Arpligner.jucer) for
[Projucer](https://juce.com/discover/projucer) is also provided if you want to
generate build files for other platforms or other plugin formats. Arpligner has
no plugin-format-specific or OS-specific code so it should be pretty
straightforward.

I originally implemented Arpligner as a Lua script for
[Protoplug](https://www.osar.fr/protoplug/), but switched to direct use of JUCE
7 for maintainability and VST3 support. The original script can be found in
[`ProtoplugArpligner.lua`](ProtoplugArpligner.lua) but I should no longer be
maintaining it.

## Tips

### General tips

- Arpligner should give more controllable results if you write/play your chords
  in their most canonical form: block chords, no inversions, without any
  octaving of notes. It will really use your chord note data as they come, it
  won't do anything fancy to try and detect which chord you are actually
  playing, what is its root, etc. But note that "more controllable" does not
  necessarily mean better ;)
- In pattern channels, only note pitches are affected. So your "pattern" notes
  will stay on the same channel, keep their velocity, etc. That means your
  patterns may contain velocity variations, pitch bends, CCs or that kind of
  things :)
- As it is working with potentially live MIDI data, Arpligner has no notion of
  things like "start point", "end point" or "loop", and thus cannot do things
  like "reset the pattern when the chord changes". Therefore, it's up to you to
  keep your chord and pattern clips in sync (or interestingly
  out-of-sync). E.g. for a regular arp "emulation", if you work with looping
  clips, just make your chord clip have a length that is a multiple of that of
  your pattern clip, so you'll repeat the same pattern several times over
  different chords.
- Besides pre-writing chord progressions or playing them live, you can use
  plugins like [Scaler 2](https://www.pluginboutique.com/products/6439) which
  speed up the process of writing chord progressions, and which can output their
  chords as MIDI and sync with the host's tempo & transport.
- Arpligner offers different behaviours for when it is receiving no notes or
  just one note on the chord channel. Default behaviour is "latch & transpose":
  it will keep using the last chord if it receives no more notes, and it will
  transpose it if it receives just one note. See [the settings
  below](#available-settings) for more info.

### Tips for Multi-channel mode

When using **Multi-channel** mode, you have two options:

- Dedicate one MIDI channel to each of your instruments. Then use only one
Arpligner instance on your **chord** track, and route **all** your MIDI data to
it, making sure that this data is properly tagged by channel. Then, split back
the MIDI data that Arpligner outputs by MIDI channel, and route each channel to
its corresponding instrument. Depending on your DAW, you may have to use 2
tracks per instrument in that fashion: one for the pattern clips for this
instrument, and one for the actual instrument.
- Place one instance of Arpligner on each of your **pattern** tracks, and route
the same **chord track** to all of them. Each instance will then receive a
pattern track on only one MIDI channel, and they will all process their patterns
according to the same chord progression. Each instance can then process pattern
data on several channels (this can be useful for multi-timbral instruments like
Kontakt or Omnisphere, or if your DAW supports layering several instruments on
the same track).

For example in Bitwig, you can put the `Note Receiver` device in a track to
route MIDI events from another track, and you can use the `Channel Map` device
in the `Source FX` section of this `Note Receiver` to make every incoming note
go to the right MIDI channel. Do not forget to deactivate the `Inputs` button in
the "Mutes" so that MIDI events from the track pass through too.

### Tips for Multi-instance mode

In **Multi-instance** mode, when some chord notes and pattern notes start at the
exact same time (which is a ubiquitous case when using MIDI clips or
sequencers), you will probably need one of the following two options (don't do
both at the same time):

- Either introduce some delay on each one of your **pattern** tracks: if your
DAW has a feature to delay MIDI notes by a few milliseconds on pattern tracks,
it can be used here (in Bitwig, that's the `Note Delay` device, which can go as
low as 10ms delay, which in my tests was enough)
- Or make sure the `Global chord track lookahead` on your **chord** track has a non-zero value:
increasing this delay parameter (in milliseconds) on your Global chord instance
should have an effect similar to the first option (but requires a change on only
one track, not on all your pattern tracks). The lookahead time on the Global
chord instance is 10ms by default. You probably will need to reload
(deactivate/reactivate) your Global chord instance if you change it, so your DAW
may register the change. This will tell your DAW that this Global chord instance
needs a bit of extra time, and it will delay all the other tracks in
consequence.

If you do not do that, your pattern instances may process their notes _before_
the chord instance has noticed that the chord has changed, and you would get
some final result which is still using the previous chord. This is perfectly
expected and cannot be avoided in situations where your MIDI is sequenced. It
comes from the fact that you have no guarantee over the order in which your DAW
will execute your plugins' logic, and therefore over which instance will notice
first some perfectly synchronized MIDI events. Therefore we need to delay a
little bit (by inaudible amounts) the pattern tracks with respect to the chord
track to make sure everything is updated in the right order. In live situations,
such perfect synchronization never occurs, so it's much less of a concern.

Note that **Multi-channel** mode does not raise this concern at all (live or
not). In that mode, given all events are processed by the same instance, I can
make sure to update the current chord prior to processing pattern notes.

## Available settings

Arpligner's GUI shows a few parameters, and exposes them to the host.

**IMPORTANT**: Unless stated otherwise, these parameters are **not** meant to be
changed while Arpligner is processing notes. If you really want to automate some
of these (for any reason), I strongly advise you to change their values **only**
during a moment in your song when no MIDI notes are being sent to Arpligner, or
you will end up with stuck notes.

| Parameter name | Default value | Possible values | Documentation |
|--------------------------------|---------------|-----------------|---------------|
|**Instance behaviour**|`[Multi-chan] Chords on chan 16`|Choose from:|The main behaviour of this Arpligner instance|
| | |`Bypass`|Do nothing and just pass all MIDI events through|
| | |`[Multi-chan] Chords on chan XX`|Sets to **Multi-channel** mode, and use channel `XX` as the chord track (and any other channel as a pattern track)|
| | |`[Multi-instance] Global chord instance`|Sets this instance as the one that receives chord notes (any MIDI input, whatever its channel) and sets the current chord for all other connected instances|
| | |`[Multi-instance] Pattern instance`|Sets this instance as a "follower" of the one set to `Global chord instance`. Any MIDI input, whatever its channel, is considered a pattern event, and will stay on the same channel|

### Chord parameters

These parameters are used *only* in Multi-chan mode or by *Chord* instances.

| Parameter name | Default value | Possible values | Documentation |
|--------------------------------|---------------|-----------------|---------------|
|**Chord notes passthrough**|`Off`|`On` or `Off`|Whether note events on the chord track should pass through instead of being consumed. Non-note events (such as CCs) on the chord track will **always** pass through. _(Ignored if instance is `Global chord instance`: notes always pass through in that case)_|
|**When no chord note**|`Latch last chord`|Choose from:|What to do when **no** note is playing on the chord track|
| | |`Latch last chord`|Keep using the previous chord that played (`Silence` if no previous chord is known)|
| | |`Silence`| Ignore the pattern notes|
| | |`Use pattern notes as final notes`|Consider the pattern "notes" as real notes, and pass them through without any change|
|**When single chord note**|`Transpose last chord`|Choose from:|What to do when a **single** note _n_ is playing on the chord track|
| | |`Transpose last chord`|Transpose last chord so that its lowest note becomes _n_ (`Silence` if no previous chord is known)|
| | |`Powerchord`|Turn _n_ into a "2-note chord": _n_ and the note a fifth above|
| | |`Use as is`|Use _n_ as just a "one-note chord". Tread carefully, the end result may go up in octaves pretty fast|
| | |`Silence`|Same as for "no chord note"|
| | |`Use pattern notes as final notes`|Same as for "no chord note"|
|**Global chord track lookahead**|`10ms`|A delay between 0 and 50ms|Only used by a Global chord instance. Triggers your DAW Plugin Delay Compensation (if above zero) to deal with perfectly synchronized chord and pattern events. See [this section](#tips-for-multi-instance-mode) for when to use this|

### Pattern parameters

These parameters are used *only* in Multi-chan mode or by *Pattern*
instances. When using the **Multi-instance** mode, you can therefore select a
different behaviour for each Pattern instance. This can be useful for instance
if some instruments are to be played live and others via MIDI sequencing, or if
different live players have different preferences.

| Parameter name | Default value | Possible values | Documentation |
|--------------------------------|---------------|-----------------|---------------|
|**Pattern notes mapping**|`Semitone to degree`|Choose from:|How to map midi note codes on pattern tracks to actual notes|
| | |`Map nothing`|No pattern note is mapped|
| | |`Semitone to degree`|Going up/down one _semitone_ in the pattern track means going up/down one degree in the chord|
| | |`White note to degree`|Going up/down one _white key_ in the pattern track means going up/down one degree in the chord. Black keys are not mapped|
| | |`Transpose from 1st degree`|Use Arpligner as a "dynamic" transposer: ignore all chord degrees besides the first (lowest) one. Pattern notes are just transposed accordingly. This allows you to play notes that are outside the current chord, but keeping your patterns centered around the reference note|
|**Reference pattern note**|`60 (C3)`|A MIDI note|On pattern tracks, the note that will always be mapped to the first (lowest) degree of the currently playing chord|
|**Unmapped pattern notes passthrough**|`Off`|`On` or `Off`|When the above mapping method leaves some notes unmapped, whether to pass them through or just silence them|

## Current limitations

- Arpligner is quite strict for now regarding the timing of notes on the chord
  channel. When your chords are played from already quantized MIDI clips or by a
  sequencer it's not a problem, but for "classical" live arp usage, i.e. when
  playing chords live against a preset pattern, it can be. So in this scenario,
  I recommend to use some sort of beat quantizer on your chord channel before
  Arpligner, and to play around with the various settings until you get a
  satisfying result.
- Note-off events are tricky to handle right, and the plugin's internal state
  will be fully refreshed every time it's reloaded, so if that happens it may
  lose track of some notes. If you ever get stuck notes, stop all MIDI data
  coming to Arpligner and disable/re-enable the plugin.
- I would love to be able to support MPE in patterns: given in Multi-instance mode you can
  use as many channels as you want for your patterns, this would allow you to
  play your patterns on any MPE instrument, with glides, polyphonic bends, expression changes, etc,
  record them and be able to keep these in your final arpeggios while re-using them over a different harmony :).
  Sadly my JUCE skills are not up to this task yet, but any help is welcome!

[^1]: I use Bitwig and the piano roll editor and MIDI operators to manipulate
    notes are just so much better and so much more flexible than everything I've
    seen in plugins

[^2]: And if someone ever makes a jazz jam session out of that, definitely
    record it and send me the video ^^
