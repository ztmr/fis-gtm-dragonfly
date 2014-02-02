ZTMR W "Hello World!",! Q
ModuleID() Q $P($ZPOS,"^",2)
SHOW
  W !,"MUMPS version is: ",$ZV,!!
  W "Code of this routine:",!!
  ZP @("^"_$$ModuleID) W "----",!!,"Let's call us:",!
  X "D ^"_$$ModuleID W !!
  W "Test globals:",!
  K ^zFoo
  F i=0:1:25 S ^zFoo(i)=$C($A("a")+i)
  ZWR ^zFoo
  W !!,"Database allocation stats:",!
  D ^%FREECNT W !
  Q
