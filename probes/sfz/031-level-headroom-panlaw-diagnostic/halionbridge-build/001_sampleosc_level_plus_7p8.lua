local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "001_sampleosc_level_plus_7p8", "001 SampleOsc Level +7.8", { sampleOscLevel = 7.8 })
end
