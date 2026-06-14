local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "002_amp_pan_left_50", "002 Amp Pan Left 50", { ampPan = -50 })
end
