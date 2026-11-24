// Args: <outDir> <hexStart> <count>. Prints `count` instructions from hexStart (absolute address),
// for checking calling conventions and argument order. outDir is unused (kept for the runner).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;

public class Disasm extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        Instruction ins = getInstructionAt(toAddr(a[1]));
        if (ins == null) ins = getInstructionAfter(toAddr(a[1]));
        long base = currentProgram.getImageBase().getOffset();
        for (int i = 0; ins != null && i < Integer.parseInt(a[2]); i++, ins = ins.getNext()) {
            println("OUT +" + Long.toHexString(ins.getAddress().getOffset() - base) + "  " + ins);
        }
    }
}
