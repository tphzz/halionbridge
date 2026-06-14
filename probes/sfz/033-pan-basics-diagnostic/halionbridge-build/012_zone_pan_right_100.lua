local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "012_zone_pan_right_100", "012 Zone Pan Right 100", { zonePan = 100 })
end
