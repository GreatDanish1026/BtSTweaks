// HDR for Life is Strange: Before the Storm Remastered: turns the game's finished SDR frame
// into scRGB (linear Rec. 709, 1.0 = 80 nits) on the add-on's 16-bit float swap chain.
// Compiled by build.ps1 with fxc (vs_5_0 / ps_5_0).
//
// The game blits its final 8-bit sRGB picture to the back buffer through an sRGB view, so on the
// float back buffer it arrives as linear light (the sRGB curve undone). The pass re-encodes it,
// decodes with gamma 2.2 (how it looks on an SDR monitor), places it at the chosen paper white
// and stretches only its top end towards the chosen peak: below the "Highlights start at" knee
// nothing changes, so the grade and the shadows stay as the game made them. Hue is kept by
// scaling RGB together.
//
// The UI keeps SDR brightness (paper white), found two ways:
//  - menus: the UI is drawn into its own texture and composited before the blit; that texture
//    (t1) is read here and its alpha counts as UI;
//  - in play: the UI is drawn into the 16-bit scene buffer just before the final tonemap. The
//    add-on copies that buffer before the UI draws (t3) and passes its final state (t2); pixels
//    that differ count as UI. That buffer is stored upside down.

cbuffer Params : register(b0)
{
    float paperWhite;  // nits for SDR white (1.0)
    float peak;        // nits the brightest SDR white is stretched to (<= paperWhite: no stretch)
    float knee;        // linear luminance where the stretch starts (0..1)
    float uiValid;     // 1: t1 holds this frame's UI layer
    float debugView;   // 0 off, 1 tint UI pixels magenta, 2 show what counts as UI
    float sceneValid;  // 1: t2/t3 hold the scene buffer after/before this frame's UI
    float2 padding;
};

Texture2D<float4> source : register(t0);       // the finished frame (linear, from the sRGB view)
Texture2D<float4> uiLayer : register(t1);      // the game's UI render target (when uiValid)
Texture2D<float4> sceneAfter : register(t2);   // the 16-bit scene buffer with the UI (when sceneValid)
Texture2D<float4> sceneBefore : register(t3);  // the same buffer before the UI (when sceneValid)

void main_vs(uint id : SV_VertexID, out float4 position : SV_Position)
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

float4 main_ps(float4 position : SV_Position) : SV_Target
{
    const int3 p = int3(position.xy, 0);
    const float3 encoded = LinearToSrgb(saturate(source.Load(p).rgb));
    float3 rgb = pow(max(encoded, 0), 2.2);

    float ui = 0;
    if (uiValid > 0)
    {
        uint w, h;
        uiLayer.GetDimensions(w, h);
        if (p.x < (int)w && p.y < (int)h)
            ui = saturate(uiLayer.Load(p).a);
    }
    if (sceneValid > 0)
    {
        uint w, h, bw, bh;
        sceneAfter.GetDimensions(w, h);
        source.GetDimensions(bw, bh);
        // The buffer may be rendered smaller, and Unity keeps it upside down (the final blit flips it).
        int3 q = int3(position.xy * float2(w, h) / float2(bw, bh), 0);
        q.y = (int)h - 1 - q.y;
        const float3 a = sceneAfter.Load(q).rgb, b = sceneBefore.Load(q).rgb;
        if (any(abs(a - b) > 0.002 + 0.01 * max(abs(a), abs(b))))
            ui = 1;
    }

    // Highlight stretch on luminance: y -> knee + (y - knee) * (1 + s * t^2), t = (y - knee) / (1 - knee).
    // Continuous with slope 1 at the knee; SDR white (1.0) lands on peak / paperWhite.
    const float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    const float top = peak / paperWhite;
    float3 stretched = rgb;
    if (top > 1 && y > knee)
    {
        const float t = (y - knee) / (1 - knee);
        const float s = knee + (y - knee) * (1 + (top - 1) / (1 - knee) * t * t);
        stretched = rgb * (s / max(y, 1e-5));
    }
    rgb = lerp(stretched, rgb, ui);  // the UI keeps SDR brightness, with soft edges

    if (debugView == 1)
        rgb = lerp(rgb, float3(1, 0, 1), 0.6 * ui);
    else if (debugView == 2)
        rgb = ui.xxx;
    return float4(rgb * (paperWhite / 80.0), 1);  // scRGB: 1.0 = 80 nits
}
