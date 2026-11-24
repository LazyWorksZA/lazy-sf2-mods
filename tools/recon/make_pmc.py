# Writes a Procmon config that keeps only StrikeFighters2.exe events and drops noise.
import sys
from procmon_parser import dump_configuration, Rule, Column, RuleAction, RuleRelation
cfg = {
    "FilterRules": [
        Rule(Column.PROCESS_NAME, RuleRelation.IS, "StrikeFighters2.exe", RuleAction.INCLUDE),
        Rule(Column.OPERATION, RuleRelation.BEGINS_WITH, "IRP_MJ_", RuleAction.EXCLUDE),
        Rule(Column.OPERATION, RuleRelation.BEGINS_WITH, "FASTIO_", RuleAction.EXCLUDE),
        Rule(Column.EVENT_CLASS, RuleRelation.IS, "Profiling", RuleAction.EXCLUDE),
    ],
    "DestructiveFilter": 0,
}
with open(sys.argv[1], "wb") as f: dump_configuration(cfg, f)
print("written", sys.argv[1])
