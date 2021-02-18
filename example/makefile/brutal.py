# USER TODO: make sure this file exists alongside the deva_includes.hpp

devastator = brutal.import_child(brutal.env('DEVA'))

@brutal.rule
def sources_from_includes_enabled(PATH):
  return brutal.os.path_within_any(PATH,'.')

@brutal.rule
def code_context(PATH):
  return devastator.code_context_base()
