#include <imgui.h>

#include <gravitaris/cgame/renderer/glow-post-process.hpp>

#include "post-process-panel.hpp"

namespace Gravitaris {

// All widgets below use Drag* rather than Slider* -- ImGui's SliderFloat/Int
// only accept manual entry via Ctrl+Click, but Drag* also supports the more
// discoverable double-click-to-type.

void DrawPostProcessPanel(GlowPostProcess& glow)
{
    // --- Glow / bloom ------------------------------------------------------
    ImGui::SeparatorText("Glow (bloom)");

    bool glowEnabled = glow.IsEnabled();
    if (ImGui::Checkbox("Enabled##glow", &glowEnabled)) {
        glow.SetEnabled(glowEnabled);
    }

    ImGui::BeginDisabled(!glowEnabled);

    float intensity = glow.GetIntensity();
    if (ImGui::DragFloat("Intensity", &intensity, 0.02f, 0.f, 6.f, "%.2f")) {
        glow.SetIntensity(intensity);
    }

    float threshold = glow.GetThreshold();
    if (ImGui::DragFloat("Threshold", &threshold, 0.005f, 0.f, 1.f, "%.3f")) {
        glow.SetThreshold(threshold);
    }
    ImGui::SetItemTooltip("Bright-pass cutoff before blur; below this doesn't bloom.");

    int blurPasses = glow.GetBlurPasses();
    if (ImGui::DragInt("Blur passes", &blurPasses, 0.1f, 0, 8)) {
        glow.SetBlurPasses(blurPasses);
    }
    ImGui::SetItemTooltip("More passes = wider, softer halo.");

    // This is a SEPARATE shimmer source from the CRT flicker/jitter below --
    // it modulates bloom intensity and runs whenever glow is enabled,
    // regardless of whether the CRT pass is on. Set to 0 for perfectly
    // steady bloom.
    float breathe = glow.GetBreatheAmplitude();
    if (ImGui::DragFloat("Breathe amount", &breathe, 0.005f, 0.f, 0.5f, "%.3f")) {
        glow.SetBreatheAmplitude(breathe);
    }
    ImGui::SetItemTooltip(
        "Bloom-intensity wobble (unstable phosphor drive). Independent of\n"
        "the CRT flicker settings below -- this is the shimmer you still see\n"
        "with those at 0, and it goes away when Glow is disabled.");

    if (ImGui::Button("Reset glow to defaults")) {
        glow.SetIntensity(GlowPostProcess::Defaults::intensity);
        glow.SetThreshold(GlowPostProcess::Defaults::threshold);
        glow.SetBlurPasses(GlowPostProcess::Defaults::blurPasses);
        glow.SetBreatheAmplitude(GlowPostProcess::Defaults::breatheAmplitude);
    }

    ImGui::EndDisabled();

    // --- CRT scanlines -----------------------------------------------------
    ImGui::SeparatorText("CRT scanlines");

    bool crtEnabled = glow.IsCrtEnabled();
    if (ImGui::Checkbox("Enabled##crt", &crtEnabled)) {
        glow.SetCrtEnabled(crtEnabled);
    }

    ImGui::BeginDisabled(!crtEnabled);

    float strength = glow.GetScanlineStrength();
    if (ImGui::DragFloat("Strength", &strength, 0.005f, 0.f, 1.f, "%.3f")) {
        glow.SetScanlineStrength(strength);
    }
    ImGui::SetItemTooltip("0 = off, 1 = fully dark troughs.");

    // Geometry is defined at the 1080p reference and scaled by window height.
    float widthPx = glow.GetScanlineWidthPx();
    if (ImGui::DragFloat("Line width (px @1080p)", &widthPx, 0.02f, 0.f, 8.f, "%.2f")) {
        glow.SetScanlineWidthPx(widthPx);
    }
    ImGui::SetItemTooltip("Dark-line thickness at 1080p (~a 2px solid core after AA).");

    float periodPx = glow.GetScanlinePeriodPx();
    if (ImGui::DragFloat("Period (px @1080p)", &periodPx, 0.05f, 1.f, 24.f, "%.2f")) {
        glow.SetScanlinePeriodPx(periodPx);
    }
    ImGui::SetItemTooltip("Line + gap at 1080p (default => 50%% duty cycle).");

    if (ImGui::Button("Reset scanlines to defaults")) {
        glow.SetScanlineStrength(GlowPostProcess::Defaults::scanlineStrength);
        glow.SetScanlineWidthPx(GlowPostProcess::Defaults::scanlineWidthPx);
        glow.SetScanlinePeriodPx(GlowPostProcess::Defaults::scanlinePeriodPx);
    }

    // --- CRT temporal flicker ----------------------------------------------
    ImGui::SeparatorText("CRT flicker / instability");
    ImGui::TextWrapped(
        "The image itself stays put -- only these effects shimmer. Setting "
        "all of these to 0 still leaves the Glow 'Breathe amount' above, "
        "which is a separate effect.");

    float flickerAmp = glow.GetFlickerAmplitude();
    if (ImGui::DragFloat("Flicker amount", &flickerAmp, 0.002f, 0.f, 0.3f, "%.3f")) {
        glow.SetFlickerAmplitude(flickerAmp);
    }
    ImGui::SetItemTooltip("Whole-frame brightness wander (phosphor/refresh flicker).");

    float flickerRate = glow.GetFlickerRate();
    if (ImGui::DragFloat("Flicker speed", &flickerRate, 0.5f, 0.f, 120.f, "%.1f")) {
        glow.SetFlickerRate(flickerRate);
    }
    ImGui::SetItemTooltip("Noise samples/sec; higher = faster shimmer.");

    float scanJitterAmp = glow.GetScanJitterAmplitude();
    if (ImGui::DragFloat("Scan jitter amount", &scanJitterAmp, 0.005f, 0.f, 1.f, "%.3f")) {
        glow.SetScanJitterAmplitude(scanJitterAmp);
    }
    ImGui::SetItemTooltip("Per-row scanline darkness wobble (unstable beam current), as a fraction of Strength.");

    float scanJitterRate = glow.GetScanJitterRate();
    if (ImGui::DragFloat("Scan jitter speed", &scanJitterRate, 0.5f, 0.f, 120.f, "%.1f")) {
        glow.SetScanJitterRate(scanJitterRate);
    }

    float phaseJitterPx = glow.GetPhaseJitterPx();
    if (ImGui::DragFloat("Raster breathing (px @1080p)", &phaseJitterPx, 0.02f, 0.f, 4.f, "%.2f")) {
        glow.SetPhaseJitterPx(phaseJitterPx);
    }
    ImGui::SetItemTooltip("Sub-pixel drift of the whole scanline grid.");

    if (ImGui::Button("Reset flicker to defaults")) {
        glow.SetFlickerAmplitude(GlowPostProcess::Defaults::flickerAmplitude);
        glow.SetFlickerRate(GlowPostProcess::Defaults::flickerRate);
        glow.SetScanJitterAmplitude(GlowPostProcess::Defaults::scanJitterAmplitude);
        glow.SetScanJitterRate(GlowPostProcess::Defaults::scanJitterRate);
        glow.SetPhaseJitterPx(GlowPostProcess::Defaults::phaseJitterPx);
    }

    ImGui::EndDisabled();
}

} // namespace Gravitaris
