' ON expr GOTO label-list.
Dim i As Byte
i = 2
On i GoTo lab1, lab2, lab3
lab1: Print 1
lab2: Print 2
lab3: Print 3
