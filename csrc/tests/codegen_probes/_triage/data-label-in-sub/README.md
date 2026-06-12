Class: data-label-in-sub (MATTERS)
Corpus rows: ad-lunam
Python's p_data calls make_label (-> declare_label, which detects collisions) BEFORE the FUNCTION_LEVEL guard, so a second in-SUB DATA emits "Label '.DATA.__DATA__N' already used". C's parser.c p_data uses symboltable_access_label (no collision check), so it omits that error.
