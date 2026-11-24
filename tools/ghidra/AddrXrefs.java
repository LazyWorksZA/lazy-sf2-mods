// Args: <outDir> <hexAddr>... (absolute addresses). Prints referencing functions and decompiles them.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.util.*;
public class AddrXrefs extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0], currentProgram.getName()); out.mkdirs();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Set<Function> done = new HashSet<>();
        for (int i = 1; i < a.length; i++) {
            Address t = toAddr(a[i]);
            Function tf = getFunctionAt(t);
            if (tf != null && done.add(tf)) write(dec, tf, out);
            for (Reference r : getReferencesTo(t)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("XREF " + a[i] + " " + r.getReferenceType() + " from " + r.getFromAddress() + " in " + (f == null ? "<none>" : f.getName()));
                if (f != null && done.add(f)) write(dec, f, out);
            }
        }
        dec.dispose();
    }
    void write(DecompInterface dec, Function f, File out) throws Exception {
        DecompileResults res = dec.decompileFunction(f, 60, monitor);
        if (res.decompileCompleted()) try (PrintWriter w = new PrintWriter(new File(out, f.getName() + ".c"))) { w.print(res.getDecompiledFunction().getC()); }
    }
}
