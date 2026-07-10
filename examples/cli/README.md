# Usage

For detailed command-line arguments, run:

```bash
./bin/sd-cli -h
```

## HuggingFace Cache Models

You can load models directly from the HuggingFace cache (`~/.cache/huggingface/hub`) using `--hf-*` options, similar to `llama.cpp`'s `-hf` flag.

The cache directory is resolved from `LLAMA_CACHE`, `HF_HUB_CACHE`, `HUGGINGFACE_HUB_CACHE`, `HF_HOME`, or `XDG_CACHE_HOME`, falling back to `~/.cache/huggingface/hub`.

### Examples

```bash
# Load a full model from HF cache
./bin/sd-cli --hf-model stabilityai/stable-diffusion-xl-base-1.0 -p "a lovely cat"

# Load a specific file by pattern
./bin/sd-cli --hf-diffusion-model stabilityai/stable-diffusion-xl-base-1.0:sdxl-base-1.0.safetensors -p "a lovely cat"

# Load components separately
./bin/sd-cli \
  --hf-diffusion-model stabilityai/stable-diffusion-xl-base-1.0 \
  --hf-vae stabilityai/stable-diffusion-xl-base-1.0 \
  -p "a lovely cat"

# Load an upscaler from HF cache
./bin/sd-cli -M upscale --hf-upscale-model amd/realesrgan-x4plus -i input.png -o output.png
```

### Available `--hf-*` options

- `--hf-model` - Full model (`-m` equivalent)
- `--hf-diffusion-model` - Standalone diffusion model
- `--hf-high-noise-diffusion-model` - High noise diffusion model
- `--hf-uncond-diffusion-model` - Unconditional diffusion model
- `--hf-clip-l` - CLIP-L text encoder
- `--hf-clip-g` - CLIP-G text encoder
- `--hf-clip-vision` - CLIP vision encoder
- `--hf-t5xxl` - T5-XXL text encoder
- `--hf-llm` - LLM text encoder
- `--hf-llm-vision` - LLM vision encoder
- `--hf-vae` - VAE model
- `--hf-audio-vae` - Audio VAE model
- `--hf-taesd` / `--hf-tae` - TAESD decoder
- `--hf-control-net` - ControlNet model
- `--hf-photo-maker` - PhotoMaker model
- `--hf-pulid-weights` - PuLID weights
- `--hf-upscale-model` - ESRGAN upscaler
- `--hf-embeddings-connectors` - LTXAV embeddings connectors

### Pattern matching

Append `:pattern` to match a specific file in the repository:

```bash
# Exact filename
./bin/sd-cli --hf-upscale-model amd/realesrgan-x4plus:RealESRGAN_x4plus.pth -i input.png -o output.png

# Substring match (case-insensitive)
./bin/sd-cli --hf-upscale-model amd/realesrgan-x4plus:.pth -i input.png -o output.png
```

If no pattern is given, the first file in the repository is used.

## Metadata mode

Metadata mode inspects PNG/JPEG container metadata without loading any model:

```bash
./bin/sd-cli -M metadata --image ./output.png
./bin/sd-cli -M metadata --image ./output.jpg --metadata-format json
./bin/sd-cli -M metadata --image ./output.png --metadata-raw
./bin/sd-cli -M metadata --image ./output.png --metadata-all
```
