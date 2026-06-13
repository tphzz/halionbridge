local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "006_amp_headroom_2", "006 Amp Headroom 2", { ampHeadroom = 2 })
end
