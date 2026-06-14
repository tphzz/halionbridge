local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "008_sampleosc_pan_right_100", "008 SampleOsc Pan Right 100", { sampleOscPan = 100 })
end
