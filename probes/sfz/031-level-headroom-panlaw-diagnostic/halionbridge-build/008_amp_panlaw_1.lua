local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "008_amp_panlaw_1", "008 Amp PanLaw 1", { ampPanLaw = 1 })
end
