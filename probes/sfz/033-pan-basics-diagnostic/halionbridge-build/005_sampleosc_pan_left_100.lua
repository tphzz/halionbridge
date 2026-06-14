local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "005_sampleosc_pan_left_100", "005 SampleOsc Pan Left 100", { sampleOscPan = -100 })
end
