local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "011_zone_pan_left_100", "011 Zone Pan Left 100", { zonePan = -100 })
end
