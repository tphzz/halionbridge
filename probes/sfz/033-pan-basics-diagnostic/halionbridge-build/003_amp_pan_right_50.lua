local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "003_amp_pan_right_50", "003 Amp Pan Right 50", { ampPan = 50 })
end
