
# Wang BASIC-2 — Practical Cheat Sheet (2200-series)

> Purpose: a compact, *LLM-friendly* reminder of Wang BASIC‑2 syntax and idioms
for writing and reading programs on a 2200VP/MVP‑class system (BASIC‑2). Examples
use program mode (numbered lines). Console prompts like `:RUN` are omitted unless
the interaction matters.

---

## 1) Program & line basics
- **Line numbers**: 0–9999, integers. Execution starts at the **lowest** line unless a `RUN n` is used.
- **Multiple statements per line**: separate with a colon `:`
  ```basic
  10 A=0 : B=1 : GOTO 100
  ```
- **Comments**: `REM` (may appear after a colon) or a single apostrophe `'` at the very start of a line (if your keyboard maps it). Use `REM` for portability.
  ```basic
  10 REM This is a note
  ```
- **Stop/End**:
  - `STOP` halts with a message and allows `CONTINUE`/`HALT/STEP` resume.
  - `END` terminates execution silently (also flushes stacks).

---

## 2) Variables and types
- **Numeric variables**: letters + digits (first char A–Z), no suffix. Scalars or arrays.
- **Alphanumeric variables (strings)**: **must** end with `$`. Default string length = **16** bytes unless set in `DIM` (see §4).
- **No explicit type declaration**; type is inferred from the name (with or without `$`).

Common system functions worth remembering:
- `ABS, INT, SQR, EXP, LOG, SIN, COS, TAN, ATN, RND, SGN, PI` (math)
- `VAL(A$)` convert substring to number; `STR(X)` create (packed) string from numeric
- `LEN(A$)`, `POS(A$, "X")`, `SEG(A$,start[,len])`, `LEFT$(A$,n)`, `RIGHT$(A$,n)`
- `CHR$(hex)` using `HEX(..)` often used with keyboard/control codes
- `SPACE` and `END` (as *functions*) report free space (see §3)

---

## 3) Memory, stacks, and free space (mental model)
Internal memory holds **Program Text**, **Work Buffer**, **Value Stack**, **Variable Table**. During execution, expressions, loops, and subroutine state push onto the **Value Stack**. Free-space queries:
- `END` (as a *statement*) ends the program and reports free space excluding the stack.
- `SPACE` (as a *function*) returns free space **including** the current stack usage (may be negative when the stack temporarily consumes part of the reserved work buffer).

Practical tip: Deep nesting of `FOR…NEXT` or recursive subroutines via `DEFFN'` increases stack usage; avoid unbounded nesting.

---

## 4) Arrays and `DIM`
- **Numeric arrays**: `DIM N(100)` or two‑dim `DIM A(10,20)`.
- **String arrays**: must specify element length if not default 16. Either attach after name or trailing after dimensions:
  ```basic
  10 DIM B$(50)        ' 50 elements, length 16 each (default)
  20 DIM A$(4,3)64     ' 4x3 elements, each 64 chars
  30 DIM C$(R,C)10     ' variable dims, fixed 10‑char elements
  ```
- **Dimension limits** (typical BASIC‑2 limits): 1‑D up to 65,535 elements; 2‑D up to 255×255. (Strings: element length 1–124.) Use `COM` to share between overlays (§5).

Advanced: You can use scalar variables to **dimension at run time** before `DIM` is resolved (e.g., first module asks the operator, next module uses those sizes).

---

## 5) Common variables (`COM`) and overlays
Use `COM` to mark variables that persist across `RUN` overlays (`LOAD`) and between modules in a chained system.
```basic
10 COM X,Y, A$(R,C)64, T$10
20 INPUT "Dims";R,C
30 LOAD "NEXTMODULE"
```
- `COM CLEAR` can flip variables between common/noncommon *without* erasing values; it does not clear memory, it only moves the “common variable pointer” up/down.
- Non‑`COM` variables are cleared by `RUN`, `CLEAR/CLEAR N`, or loading a new module.

---

## 6) Input: `INPUT`, `LINPUT`, and `KEYIN`
**INPUT** parses comma‑separated values; a prompt literal is optional:
```basic
10 INPUT "VALUE OF A,B"; A,B
```
- Strings entered without quotes are terminated by comma or RETURN; to include commas or leading blanks, enter the value in quotes.

**LINPUT** reads an entire line (including blanks) into a string variable; you then parse it:
```basic
10 LINPUT A$        ' read raw line with spaces/commas intact
```

**KEYIN** reads a **single character** (or branches on a Special Function key); useful for menus and character‑by‑character editors:
```basic
10 KEYIN K$         ' get one char into K$; continue at next statement
20 IF K$="Y" THEN 60
30 IF K$="N" THEN 80
40 GOTO 10
```

Special Function keys (SF 00–31) can be used with `KEYIN` to branch directly to a line label/number; see §9 for defining them.

---

## 7) Output: `PRINT`, separators, zones
```basic
10 PRINT "HELLO"; 123; A$     ' semicolon keeps printing on the same line
20 PRINT "A",  "B",  "C"      ' comma advances to the next output zone
30 PRINT                        ' blank PRINT emits end‑of‑line (CR/LF or device EOL)
```
- **Semicolon (`;`)** suppresses the end‑of‑line and the zone jump.
- **Comma (`,`)** moves to the next *print zone* (device‑defined columns). Use `SELECT PRINT` to choose device/width; don’t rely on a single universal zone size across devices.
- **Trailing `;` or `,`** keeps the next `PRINT` on the same physical line/zone.
- `PRINT AT r,c; ...` positions on screen devices that support it.

---

## 8) Formatted printing: `PRINTUSING` (+ image lines)
Formatted output is a two‑step pattern:
1) Define an **image line** (format) using the **percent sign `%`** in a separate program line.
2) Emit values with `PRINTUSING` referencing that image line number.
Minimal pattern:
```basic
100 %  ####.##  $$,$$9  !!!!!!!!!!!!!!!!!!!!!   ' define pictures/text slots
110 PRINTUSING 100, 3.14159, 1234.5, "Title here"
```
Notes
- You can maintain multiple image lines and reuse them across the program.
- `PRINTUSING TO #n, ...` targets another device/file if you’ve SELECTed it.
- For full picture‑code details (digits, sign control, justification, literal embedding), see the “Image (%)” and `PRINTUSING` sections in the manual.

---

## 9) Function keys & quick subroutines — `DEFFN'` and `GOSUB'`
There are **32 Special Function keys** (0–31) giving fast access to *text entry* macros **or** to *subroutine entry points*.

### a) Text entry macros
```basic
10 DEFFN'12 "HEX("      ' when SF 12 is pressed during text entry, inserts HEX(
```
- You can concatenate pieces using semicolons; `HEX(0D)` terminates with a carriage return (useful for keystroke sequences).

### b) Subroutine entry points
```basic
10 DEFFN'2(A,B$)        ' declare SF 2 as a subroutine entry, with parameters
20 ... main program ...
100 DEFFN'2(A,B$)
110 REM work with A and B$
190 RETURN
```
- Enter from a program with `GOSUB' 2 (A,B$)` **or** by pressing the SF key.
- If called from the keyboard (during an `INPUT`), arguments can be typed before pressing the key; the system prompts for any missing arguments.
- On `RETURN`, control goes back to the caller (or to console input if invoked from the keyboard).
- **Do not** leave pending `RETURN`s for SF‑invoked subroutines (stack can overflow).

---

## 10) User‑defined numeric functions — `DEFFN` / `FNx(...)`
Define a pure **numeric** function of one dummy variable:
```basic
10 DEFFN A(X) = X*(2-X)
20 PRINT FNA(6)          ' evaluates the expression with X=6
```
- Function names are single letter or digit (`A`–`Z`, `0`–`9`).
- Max **nesting** of referenced user functions is limited (avoid mutual recursion).

---

## 11) Control flow
### `IF … THEN` (single‑line form)
```basic
10 IF X>0 THEN PRINT "POSITIVE" : GOTO 100
```
- One or more statements may follow `THEN` (colon‑separated).

### Structured multi‑line IF
```basic
10 IF X>100 THEN
20    PRINT "BIG"
30    S=S+1
40 IF END THEN
```
- The block ends at `IF END THEN`. (Think of it as `ENDIF`.)

### `ON … GOTO` / `ON … GOSUB`
```basic
10 ON K GOTO 100,200,300
20 ON K GOSUB 900,990
```
- If the index is <1 or >list length, no branch occurs (execution continues).

### Loops — `FOR…TO…[STEP]` / `NEXT`
```basic
10 FOR I=1 TO 10 STEP 2
20   PRINT I
30 NEXT I
```
- `STEP` defaults to `+1`; negative step reverses the termination test.
- It’s legal to `RETURN` out of an inner loop inside a subroutine; the interpreter clears the loop parameters for that block. Avoid branching *into the middle* of a loop.

### Unconditional
- `GOTO n`, `GOSUB n` … `RETURN` (also `RETURN CLEAR` to clear the subroutine return info without jumping back).

---

## 12) Data tables — `DATA`, `READ`, `RESTORE`
```basic
10 READ A, B$, X
20 DATA 5, "BOSTON", 3.14
30 RESTORE        ' (optional) reset pointer to first DATA
```
- READ pulls sequentially through all DATA statements in the program text. Use `RESTORE` to reset the pointer (or `RESTORE line` to jump to a specific DATA block).

---

## 13) Math, trig mode & rounding
- Trig functions follow the current **mode** (degrees, radians, grads) selected with the `SELECT` statement (see §14). Default is device/system default; set it explicitly in math‑heavy code.
- Rounding/truncation for formatted output depends on picture spec (`PRINTUSING`) and device output width; for raw `PRINT` the internal numeric precision is maintained until converted to text.

---

## 14) The `SELECT` statement (device & options)
`SELECT` is a multipurpose *environment control* statement. Common patterns:
```basic
10 SELECT PRINT            ' choose the console printer (or set its params)
20 SELECT PRINT L 132      ' line width = 132 columns (device dependent)
30 SELECT PRINT P          ' pause at end of page (toggle/pause‑on)
40 SELECT INPUT            ' choose the console keyboard as input device
50 SELECT TRIG DEG         ' trig mode: DEG|RAD|GRAD
60 SELECT ERROR RESUME 100 ' trap next error; resume at line 100
```
Other capabilities include setting line/page length, output device addresses, and (on systems with the programmable interrupt feature) defining interrupts. Exact keywords vary by device; favor `SELECT` near program start to make output predictable.

---

## 15) Strings & substrings (fast patterns)
```basic
10 A$ = "SMITH, JOHN"
20 LAST$ = RIGHT$(A$, LEN(A$)-POS(A$, ",")-1)
30 FIRST$ = LEFT$(A$, POS(A$, ",")-1)
40 PRINT FIRST$;" ";LAST$
```
- Use `LEN`, `POS`, `LEFT$`, `RIGHT$`, `SEG` to slice without copying more than necessary.
- For packed/binary work (disk, BCD, bit‑ops) see the binary/decimal operators (`ADD[C]`, `SUB[C]`, `DAC`, `DSC`) and `PACK/UNPACK` in the conversion chapter.

---

## 16) Errors & recovery
- Unhandled runtime errors stop with a message and line number. You can intercept via `SELECT ERROR` (to change the response) or by using `ERROR` (statement) in controlled contexts.
- Handy development idioms:
  ```basic
  900 PRINT "ERR AT";ERL; " CODE";ERR : STOP
  ```
  where `ERR`/`ERL` (if available on your revision) expose last error & line.

---

## 17) Disk/files (very brief)
The original 2200 BASIC‑2 uses device addresses (`/taa` or `#n`) and file numbers for I/O. Common verbs include `OPEN`, `CLOSE`, `INPUT #`, `PRINT #`, `MOVE`, and higher‑level disk utilities documented in the Disk Reference. When using `PRINTUSING TO` or `INPUT #`, remember to `SELECT` the device first or address it explicitly with `#`/`/taa` forms.

---

## 18) Quick patterns

### Menu with single‑key navigation
```basic
10 PRINT "1) LIST";:PRINT "2) ADD";:PRINT "Q) QUIT";
20 KEYIN K$
30 ON POS("12Q",K$) GOTO 100,200,900
40 GOTO 20
100 GOSUB 1000 : GOTO 20
200 GOSUB 2000 : GOTO 20
900 END
```

### Validating input format (e.g., ZIP code all digits)
```basic
10 LINPUT Z$
20 IF LEN(Z$)<>5 THEN PRINT "5 digits";:GOTO 10
30 FOR I=1 TO 5: IF SEG(Z$,I,1)<"0" OR SEG(Z$,I,1)>"9" THEN PRINT "digits only";:GOTO 10
40 NEXT I
50 PRINT "OK"
```

### Using a Special Function key to total numbers on demand
```basic
10 DIM A(30)
20 N=0
30 INPUT "AMOUNT";A(N) : N=N+1 : GOTO 30
50 DEFFN'2
60 T=0 : FOR I=0 TO N-1 : T=T+A(I) : NEXT I
70 PRINT "TOTAL=";T
80 RETURN
```

---

### What to check when porting code **to** BASIC‑2
- String default length (16) and explicit element lengths in `DIM` for string arrays.
- Zone behavior of `PRINT` on your target device; set widths with `SELECT PRINT`.
- Replace single‑line `IF` chains with structured `IF…IF END THEN` blocks where clarity matters.
- Use `DEFFN'`/`GOSUB'` rather than `GOTO` for SF‑key driven workflows.
- Ensure arrays and `COM` layouts are agreed across modules (same dimensions/order).

---

**End of cheat sheet.**
