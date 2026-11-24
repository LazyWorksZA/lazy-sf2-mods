// Args: <outDir> <ImportName>...  For each external (imported) function whose name matches,
// prints every caller (direct or through thunks / IAT pointers) and decompiles the callers into
// outDir/<program>/imp_<name>/
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ImportXrefs extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        SymbolTable st = currentProgram.getSymbolTable();
        for (int i = 1; i < a.length; i++) {
            String name = a[i];
            File out = new File(a[0], currentProgram.getName() + "/imp_" + name); out.mkdirs();
            Set<Function> done = new HashSet<>();
            List<Address> targets = new ArrayList<>();
            for (Symbol s : st.getExternalSymbols()) if (s.getName().equals(name)) targets.add(s.getAddress());
            // IAT pointers and thunks that reference the external also count as targets
            List<Address> extra = new ArrayList<>();
            for (Address t : targets) for (Reference r : getReferencesTo(t)) {
                extra.add(r.getFromAddress());
                Function th = getFunctionContaining(r.getFromAddress());
                if (th != null && th.isThunk()) extra.add(th.getEntryPoint());
            }
            targets.addAll(extra);
            for (Address t : targets) for (Reference r : getReferencesTo(t)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null || f.isThunk()) continue;
                println("XREF " + currentProgram.getName() + " " + name + " from +" + Long.toHexString(r.getFromAddress().getOffset() - base)
                        + " in " + f.getName() + "@+" + Long.toHexString(f.getEntryPoint().getOffset() - base));
                if (done.add(f)) {
                    DecompileResults res = dec.decompileFunction(f, 60, monitor);
                    if (res.decompileCompleted()) try (PrintWriter w = new PrintWriter(new File(out, f.getName() + ".c"))) { w.print(res.getDecompiledFunction().getC()); }
                }
            }
        }
        dec.dispose();
    }
}
