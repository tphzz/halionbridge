local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "001_amp_pan_left_100", "001 Amp Pan Left 100", { ampPan = -100 })
end
