# CheapGlk with LLM-Powered Natural Language Input

This is an experimental fork of CheapGlk to explore how large language models can enhance interactive fiction gameplay through intelligent command interpretation.

## What Is This?

This is CheapGlk (the simplest Glk I/O library) updated with transparent LLM-based natural language processing. It allows players to use conversational language instead of precise parser commands when playing interactive fiction games.

### The Core Idea

Interactive fiction traditionally requires players to learn specific command syntax. "North" or "Take key" work fine, but "let's check out the building" or "Take it" (referring to the key) don't, unless the game specifically has code to allow that. 

But the game doesn't actually care what words the player uses to communicate their intent. It just needs a normalized command like "inventory" or "wear coat". The interpretation of what someone actually meant is just an I/O problem. It's not a game logic problem.

This fork implements a prototype of putting the LLM at the Glk layer. A player types "go to the bedroom", the I/O layer grabs that, asks the LLM what they probably meant given the scene context, gets back "north", and passes that to the game. This way the game never needs to know that any of this happened. It just receives "north" like it always does.

So when you type:

- "what do I have?" it gets converted to "inventory"
- "let's go to the bedroom" it gets converted to "north" (or any appropriate direction)
- "wear my winter coat" it gets converted to "wear coat"

The intention is to keep the game unaware of any of this and thus keep all existing games playable.

One fun/interesting aspect is that because of the nature of LLMs, it's possible to to use a totally different language (or even transliteration) and the input will be converted correctly:

- "öppna dörren" gets converted to "open door"
- "efta7 el bab" gets converted to "open door"

## Architecture

```
Player Input: "go to the bedroom"
       ↓
  [Glk I/O Layer]
  - Captures input
  - Sends to LLM with scene context
  - Gets back: "north"
       ↓
Game VM receives: "north"
  - Processes normally
       ↓
Output rendered to player
```

The LLM integration lives entirely in the Glk library (I/O layer), never touching the VM. This way we can leave Glk to handle input and output, including natural language interpretation, and the VM to execute the game logic, completely unaware of how input was obtained.

## Examples

The LLM reads scene descriptions to understand room layouts and connect player intent to actual commands:

```
Living Room
A door to the north leads to the bedroom.

> go to the bedroom
[LLM: "go to the bedroom" -> "north"]
```

The system understands the current position to avoid nonsensical interpretations. If you're already in a location, asking to go there gets caught:

```
Back Alley

> go to the alley
[LLM: "go to the alley" -> ""]
{Default game response}
```

Some games ask the user to input something that's supposed to be treated like a raw string, such as their name. In these cases, the player can surround their input with brackets [] to skip the LLM interpretation layer.

```
What is your name, brave Knight? [Beshr]

"Wake up, Beshr, wake up!" Someone is shaking your shoulder.
```

Some other interesting stuff:

```
...

You can see the box (closed), a mirror and a metal door here.

> öppna lådan
[LLM: "öppna lådan" -> "open box"]
You open the box, revealing a pistol.

> take it
[LLM: "take it" -> "take pistol"]
Taken.

> efta7 el bab
[LLM: "efta7 el bab" -> "open door"]
You open the metal door.

```

## Configuration

Create `~/.glk_llm.conf`:

```ini
# Enable LLM processing (0=disabled, 1=enabled)
enabled=1

# API endpoint (OpenAI-compatible)
api_endpoint=https://openrouter.ai/api/v1/chat/completions

# API key from your provider
api_key=sk-your-api-key-here

# Model to use
model=gpt-4

# Number of recent game output lines to include as context (0-20)
context_lines=10

# Request timeout in milliseconds
timeout_ms=5000

# Show interpretation in brackets (0=silent, 1=show)
echo_interpretation=1
```

### Supported Providers

**OpenAI:**
```ini
api_endpoint=https://api.openai.com/v1/chat/completions
api_key=sk-your-openai-key
model=gpt-4
```

**OpenRouter:**
```ini
api_endpoint=https://openrouter.ai/api/v1/chat/completions
api_key=sk-or-v1-your-key
model=anthropic/claude-3-5-sonnet
```

**Local Ollama:**
```ini
api_endpoint=http://localhost:11434/v1/chat/completions
api_key=dummy
model=llama2
```

## Building

### Native Build (CLI)

Requires OpenSSL for HTTPS API calls:

```bash
# macOS
brew install openssl

# Ubuntu/Debian
apt-get install libssl-dev

# Build
make
```

Then rebuild any IF interpreters (e.g., glulxe) that link against this library.

### WebAssembly Build (Browser)

Requires [Emscripten SDK](https://emscripten.org/):

```bash
# Install Emscripten
cd ..
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh

# Build
cd ../cheapglk
make wasm
```

The WASM build creates `libcheapglk.wasm.a` which can be linked by WASM-compiled interpreters like glulxe.

For browser usage, see the glulxe repository for the complete web interface.

### Cleaning

```bash
make clean        # Clean native build
make clean-wasm   # Clean WASM build only
```

### Limitations

There are obviously some issues that might make the experience not that great for now:

- Interpretation isn't always perfect or consistent, depending on the model you use and game you're playing
- LLM calls add latency (typically 500ms-2s)
- API costs accumulate with usage
- Depends on external service availability
- Model quality varies significantly, I've had some good results with `google/gemini-2.5-flash` but smaller models might require a lot of finetuning to be useful (but this might be the best direction for a totally offline experience).

## License

MIT License, same as the original CheapGlk.

## Credits

- **CheapGlk**: Andrew Plotkin
- **LLM-based Interpretation Experiment**: Beshr Kayali Reinholdsson
