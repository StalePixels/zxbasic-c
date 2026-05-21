' ON expr GOSUB label-list.
Dim i As Byte
i = 1
On i GoSub lab1, lab2
Print 99
End
lab1: Print 1
Return
lab2: Print 2
Return
