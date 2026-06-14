local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "006_sampleosc_pan_left_50", "006 SampleOsc Pan Left 50", { sampleOscPan = -50 })
end
