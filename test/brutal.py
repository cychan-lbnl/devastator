@brutal.rule
def code_context(PATH):
  return (code_context.parent_rule(PATH) |
          CodeContext(pp_angle_dirs = [brutal.here()]))
