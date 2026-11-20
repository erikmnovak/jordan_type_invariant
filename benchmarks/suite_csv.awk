# Read the C++ catalogue or a single process's measurement rows. Keep checksums
# as text: the benchmark values can exceed the exact integer range of doubles.
function invalid(reason) {
    print "Invalid benchmark CSV: " reason > "/dev/stderr"
    bad = 1
    exit 1
}
BEGIN { field_pattern = "([^,\"]*)|(\"([^\"]|\"\")*\")" }
{
    # A quoted file path may contain commas, doubled quotes or a newline.
    record = $0
    part = $0
    quotes = gsub(/"/, "", part)
    while (quotes % 2) {
        if (getline <= 0) invalid("unterminated quoted field")
        record = record "\n" $0
        part = $0
        quotes += gsub(/"/, "", part)
    }
    sub(/\r$/, "", record)
    n = patsplit(record, field, field_pattern, separator)
    if (separator[0] != "" || separator[n] != "") invalid("malformed field")
    for (i = 1; i < n; i++) if (separator[i] != ",") invalid("malformed field")
    for (i = 1; i <= n; i++) {
        if (substr(field[i], 1, 1) == "\"") {
            field[i] = substr(field[i], 2, length(field[i]) - 2)
            gsub(/""/, "\"", field[i])
        }
    }
    if (++rows == 1) {
        width = n
        if (mode == "list") {
            if (record != "workload,presentation,slice_file,seed") invalid("catalogue header")
        } else {
            if (field[1] != "workload" || field[n] != "checksum") invalid("measurement header")
            for (i = 1; i <= n; i++) if (field[i] == "method") method_col = i
            print "repeat," record
        }
        next
    }
    if (n != width || field[1] !~ /^[a-zA-Z0-9_-]+$/) invalid("unexpected columns or workload")
    if (mode == "list") {
        if (seen[field[1]]++ || (field[4] != "" && field[4] !~ /^[0-9]+$/)) invalid("catalogue entry")
        for (i = 1; i <= n; i++) printf "%s%c", field[i], 0
    } else {
        if (field[1] != expected || field[n] !~ /^[0-9]+$/) invalid("workload or checksum")
        method = method_col ? field[method_col] : "jordan_type"
        if (method_col && method !~ /^(rank_blocks|streaming|filtered_ranks)$/) invalid("unknown method")
        if (seen[method]++) invalid("duplicate method")
        printf "%s,%s\n", method, field[n] > sums
        print repeat "," record
    }
}
END {
    if (!bad && (rows < 2 || (mode == "measure" && rows != (method_col ? 4 : 2))))
        invalid("incomplete output")
}
