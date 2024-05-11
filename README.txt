Welcome to the most advanced Julia's set visualiser ever known to mankind. 

LAUNCH
    two programs need to be launched:
    ./module
    ./prgsem-main

ARGUMENTS
    if you want to modify the code with your own arguments, you can launch the prgsem-main 
    with:
        ./prgsem-main <c_re> <c_im> <im> <re> <d_im> <d_re> <resolution>
        
        where <c_re> is real constant of C in julia set (double)
              <c_im> is imaginary part of constant C in julia set (double)
              <im>   is a start of computation (imaginary coordinate) (double)
              <re>   is a start of computation (real coordinate) (double)
              <d_im> is a step per pixel of imaginary part of number (double)
              <d_re> is a step per pixel of real part of number (double)
              <resolution> is a resolution of the render (specify by integer number from selection below):
                    '1' 320x240 - xwin_close makes weird error i dont understand :(
                    '2' 640x480
            

        NOTE: you need to include all parameters when launching code - otherwise the program will 
              run with default values!
    


IN APP CONTROL:
    press:
        'r' - reset cid for remote computation
        'g' - get firmware version of the computation module
        'c' - compute and visualise julia set locally
        's' - send computation data to computation module
        '1' - wake up compuation module and draw results 
        'l' - redraw default color 
        'd' - download current window as PNG (*BONUS)
        'q' - escape the program - close all threads - clean exits both module and main
        'a' - abort current computation (for 1 - local computation cant be aborted - takes 
              less time than human reaction time). remote computing can be aborted either from main or
              from module. 


Safety note: 
    please DO NOT press all keys at once.. the program shall be safe, but during testing it liked
    to spawn black holes all around the server room. :-)