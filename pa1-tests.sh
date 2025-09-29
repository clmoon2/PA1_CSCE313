#!/usr/bin/env bash
set -euo pipefail

COLOUR=0
if [[ $COLOUR -eq 0 ]]; then
  ORANGE='\033[0;33m'; GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
else
  ORANGE='\033[0m'; GREEN='\033[0m'; RED='\033[0m'; NC='\033[0m'
fi
SCORE=0

make clean && make

echo -e "\nStart testing"

# Test 1: client runs cleanly
if ./client; then
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+15))
else
  echo -e "  ${RED}Failed${NC}"
fi
echo -e "\nSCORE: ${SCORE}/85\n"

# Test 2a: single datapoint (expects a specific sentence on stdout)
expected_output="For person 1, at time 0.004, the value of ecg 1 is 0.68"
program_output=$(./client -p 1 -t 0.004 -e 1 2>&1 || true)
if [[ "$program_output" == *"$expected_output"* ]]; then
  echo -e " ${GREEN}Passed${NC}"; SCORE=$((SCORE+5))
else
  echo -e " ${RED}Failed${NC}"
fi
echo -e "\nSCORE: ${SCORE}/85\n"

# Test 2b: first 1000 rows to received/x1.csv
./client -p 1
if diff -qwB <(head -n 1000 BIMDC/1.csv) received/x1.csv >/dev/null; then
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+10))
else
  echo -e "  ${RED}Failed${NC}"
fi
echo -e "\nSCORE: ${SCORE}/85\n"

# Test 3: file transfers
dd if=/dev/zero of=BIMDC/test.bin bs=1024k count=10 status=none
./client -f test.bin
if diff -qwB BIMDC/test.bin received/test.bin >/dev/null; then
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+20))
else
  echo -e "  ${RED}Failed${NC}"
fi
rm -f BIMDC/test.bin received/test.bin

truncate -s 10000000 BIMDC/test.bin
./client -f test.bin
if diff -qwB BIMDC/test.bin received/test.bin >/dev/null; then
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+10))
else
  echo -e "  ${RED}Failed${NC}"
fi
rm -f BIMDC/test.bin received/test.bin

truncate -s 100000000 BIMDC/test.bin
./client -f test.bin
if diff -qwB BIMDC/test.bin received/test.bin >/dev/null; then
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+5))
else
  echo -e "  ${RED}Failed${NC}"
fi
rm -f BIMDC/test.bin received/test.bin

# Test 4: new channel + file; detect extra FIFOs
count_fifos() { find . -maxdepth 1 -name "*fifo*" -type p 2>/dev/null | wc -l; }
dd if=/dev/zero of=BIMDC/test.bin bs=1024k count=10 status=none
./client -c -f test.bin &
CLIENT_PID=$!

max_count=0
while kill -0 "$CLIENT_PID" 2>/dev/null; do
  c=$(count_fifos)
  [[ $c -gt $max_count ]] && max_count=$c
  sleep 0.05
done
wait "$CLIENT_PID" || true

if [[ $max_count -gt 2 ]] && diff -qwB BIMDC/test.bin received/test.bin >/dev/null; then
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+15))
else
  echo -e " ${RED}Failed (no new channel detected)${NC}"
fi
rm -f BIMDC/test.bin received/test.bin
echo -e "\nSCORE: ${SCORE}/85\n"

# Test 5: no leftovers in repo root
if ls fifo* data*_* *.tst *.o *.csv *.log 1>/dev/null 2>&1; then
  echo -e "  ${RED}Failed${NC}"
else
  echo -e "  ${GREEN}Passed${NC}"; SCORE=$((SCORE+5))
fi

echo -e "\nSCORE: ${SCORE}/85\n"
