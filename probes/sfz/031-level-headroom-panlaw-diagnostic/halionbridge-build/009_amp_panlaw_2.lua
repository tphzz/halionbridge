local variant = require("level_headroom_panlaw_variant")

return function(ctx)
    return variant.build(ctx, "009_amp_panlaw_2", "009 Amp PanLaw 2", { ampPanLaw = 2 })
end
