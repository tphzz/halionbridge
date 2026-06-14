local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "004_amp_pan_right_100", "004 Amp Pan Right 100", { ampPan = 100 })
end
