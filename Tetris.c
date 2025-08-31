#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xscugic.h"   //Definitii functii driver intreruperi GIC (generic interrupt controller)
#include "xaxivdma.h" //Definitii functii driver VDMA
#include "xgpio.h"    //Definitii functii GPIO
#include "sleep.h"
#include "xtime_l.h"
#include "xil_exception.h"
#include "xil_util.h"
#include "xil_types.h"
#include "xuartps.h"

//=================INITIALIZARE

//Adresele de memorie DDR
#define DDR_BASE_ADDR	XPAR_PS7_DDR_0_S_AXI_BASEADDR
#define DDR_HIGH_ADDR	XPAR_PS7_DDR_0_S_AXI_HIGHADDR
#define MEM_BASE_ADDR	(DDR_BASE_ADDR + 0x01000000)
#define MEM_HIGH_ADDR	DDR_HIGH_ADDR
#define MEM_SPACE		(MEM_HIGH_ADDR - MEM_BASE_ADDR)

//Adresele canalelor pentru citire si scriere
#define READ_ADDRESS_BASE	MEM_BASE_ADDR
//#define WRITE_ADDRESS_BASE	(MEM_BASE_ADDR + 0x02000000)

//Definitii pentru LED-uri
#define LED_ID			XPAR_AXI_GPIO_LED_DEVICE_ID
#define LED_CHANNEL 	1

//Definitii pentru BUTOANE
#define BTN_ID			XPAR_AXI_GPIO_BTN_DEVICE_ID
#define BTN_CHANNEL		1
#define BTN_MASK 		0b1111

//Definitie pentru adresa UART
#define UART_BASEADDR XPAR_XUARTPS_0_BASEADDR

//Definitii pentru VDMA
#define VDMA_ID			XPAR_AXI_VDMA_0_DEVICE_ID
//Definitii pt intreruperi
#define INTC_DEVICE_ID	XPAR_SCUGIC_SINGLE_DEVICE_ID
		// !!NOTA!! Definitia de la ID INTR sunt outdated in exemplul de la Xilinx!
		//(Vezi xparameters.h pt definitiile corecte SCUGIC precum e cea de mai jos)
#define READ_INTR_ID	XPAR_FABRIC_AXI_VDMA_0_MM2S_INTROUT_INTR
#define DELAY_TIMER_COUNTER 0

//Definitii pt frame
#define FRAME_HORIZONTAL_LEN	640*3   // 640 pixeli *3 octeti fiecare
#define FRAME_VERTICAL_LEN		480    // 480 pixeli
#define FRAME_SIZE				640*480*3 // Dimensiunea totala a frame-ului

#define FRAME_START_OFFSET    0
#define FRAME_HORIZONTAL_SIZE 640
#define FRAME_VERTICAL_SIZE   480
#define PixelBytes 			  3

XScuGic Intc;	 //Instanta pt intrerupere
XGpio LED, BTN;	 //Instanta pt LED, Butoane
XAxiVdma VDMA;   //Instanta pt VDMA
static XAxiVdma_DmaSetup ReadCfg; //Instanta configurare VDMA

//Pt a seta numarul dorit de frame-uri stocate
volatile static u16 ReadCount;

void InitComponents();
void ReadSetup(XAxiVdma *InstancePtr);

static int SetupIntrSystem(XAxiVdma *AxiVdmaPtr, u16 ReadIntrId);
static void DisableIntrSystem(u16 ReadIntrId);
//Callback-ul intreruperilor
static void ReadCallBack();
static void ReadErrorCallBack();

//===============GRAFICA
#define TILE_SIZE       14
#define GRID_WIDTH      10
#define GRID_HEIGHT     20

#define PLAYFIELD_WIDTH     (GRID_WIDTH * TILE_SIZE)  // 140 pixeli
#define PLAYFIELD_HEIGHT    (GRID_HEIGHT * TILE_SIZE) // 280 pixeli

#define PLAYFIELD_OFFSET_H     ((FRAME_HORIZONTAL_SIZE - PLAYFIELD_WIDTH) / 2) // X: 250
#define PLAYFIELD_OFFSET_V     ((FRAME_VERTICAL_SIZE - PLAYFIELD_HEIGHT) / 2) // Y: 100

void MovingSquare(int squareSize, int squarePosV, int *squarePosH, int *direction);
void DrawPlayfieldBorders();
void DrawPlayfield();
void DrawTetromino();

u32 GetHexColor(char name);

/*Bufferele pentru afisare*/
unsigned char backBuffer[FRAME_SIZE];
unsigned char frontBuffer[FRAME_SIZE];

//==============LOGICA JOC
#define GRID_M_COLS GRID_WIDTH
#define GRID_M_ROWS (GRID_HEIGHT+2)
#define TETROMINO_M_SIZE 4
#define NO_TETROMINOS 7
#define MAX_LEVEL 10
#define LINES_THRESHOLD 5
#define SPEED_DECREASE 6

typedef struct {
	char name;
	int matrix[TETROMINO_M_SIZE][TETROMINO_M_SIZE];
	int row;
	int col;
} Tetromino;

Tetromino GenerateNextTetromino();
void ShuffleSequence(char* array, int size);
char GetNextTetrName();
void* GetTetrominoMatrix(char name);
void SetRndSeed();
int ValidMove(int row, int col, int matrix[TETROMINO_M_SIZE][TETROMINO_M_SIZE]);
int PlaceTetromino();
void CheckLines();
void InputCotrol();
void RotateTetrominoMatrix();
void ClearTerminal();
void UpdateLevel();

/* Folosim o matrice 2d pt a tine evidenta a ce exista in fiecare
 * bloc din zona de joc. Cu inaltime de 22 numerotat de sus in jos.
 * Primele 2 coloane (0, 1) nu vor fi vizibile pe ecran.*/
int playfield[GRID_M_ROWS][GRID_M_COLS];
Tetromino crtTetromino;
char tetrominoSequence[NO_TETROMINOS];
int sequenceIndex = 0;
int gameScore = 0;
int frameCount = 0;
int gameSpeed = 63;
int totalLinesCleared = 0;
int currentLevel = 1;

/*============================ Main ==============================*/

int main(void)
{	init_platform();

    SetRndSeed();
    InitComponents();

    //Initializez cu 0 matricea la inceput de joc
    memset(playfield, 0, sizeof(playfield));
    DrawPlayfieldBorders(); // Desenez graficile marginilor jocului

    // Se genereaza prima piesa tetromino
    crtTetromino = GenerateNextTetromino();
    int gameOver = 0; // Variabila pt statusul jocului

    // Intarziere pentru a da timp ecranului sa porneasca inainte de afisare
    sleep(4);
    ClearTerminal();
    xil_printf("\n\n\n\n\n\n\n\n\t\t\t\tJoc inceput! \r\n");

	while(1) {
    	// Primirea si executarea controlului de la tastatura
    	InputCotrol();

    	// Inatarziem coborarea piesei prin adunarea repetata a unui contor
    	// Numarul reprezinta frame-urile afisate pana cand piesa coboara cu un bloc(37)
    	if(++frameCount > gameSpeed){
    		frameCount = 0;
	        crtTetromino.row++;

	        // Daca piesa se loveste de alta piesa sau de campul de joc aceasta se plaseaza
	       	if (!ValidMove(crtTetromino.row, crtTetromino.col, crtTetromino.matrix)) {
	       		crtTetromino.row--;

	       	    // Daca piesa iese inafara campului, jocul s-a terminat
	       	    gameOver = PlaceTetromino();
	       	    if (gameOver) {
	       	    	xil_printf("\n\t\t\t\tJoc incheiat! \n");
	       	    	break;
	       	    }
	       	}
    	}

        // Desenarea piesei pe noua pozitie
        DrawTetromino();
        // O intarziere care are rolul de a regla afisajul pe monitor,
        // cu o frecventa de 80HZ pt a nu fi vizibila de catre utilizator
        usleep(12500);
        // Resetam grafica campului de joc pentru o noua cadere a piesei
    	DrawPlayfield();

	}

	cleanup_platform();
    return 0;
}

/*============================ Grafica ==============================*/

/* Returneaza culoarea piesei curente pentru a colora grafica acesteia*/
u32 GetHexColor(char name){
	switch(name){
	case 'I': return 0x00FFFF; // Cyan
    case 'O': return 0xFFFF00; // Yellow
    case 'T': return 0x800080; // Purple
    case 'S': return 0x00FF00; // Green
    case 'Z': return 0xFF0000; // Red
    case 'J': return 0x0000FF; // Blue
    case 'L': return 0xFFA500; // Orange
    default:  return 0xFFFFFF; // White
	}
}

/* Functie pentru a desena zona de joc utilizabila.
 * Avem un offset si dimensiuni prestabilite pentru camp, alese in asa fel incat acesta sa fie usor vizibil la fel si piesele din acesta.
 *
 * La desenarea oricarei forme pe linia orizontala se ia in calcul numarul de bytes (octeti) de pe fiecare pixel, responsabili pt culoare. */
void DrawPlayfieldBorders(){
	u32 color = GetHexColor('W'); //Culoarea alb din default
	// Parcurgerea matricii de sus in jos si de la stanga la dreapta
    for (int y = PLAYFIELD_OFFSET_V; y < PLAYFIELD_OFFSET_V + PLAYFIELD_HEIGHT + TILE_SIZE; y++) {
        for (int x = (PLAYFIELD_OFFSET_H - TILE_SIZE) * PixelBytes; x < (PLAYFIELD_OFFSET_H + PLAYFIELD_WIDTH + TILE_SIZE) * PixelBytes; x += PixelBytes) {

            int index = y * FRAME_HORIZONTAL_SIZE * PixelBytes + x;

            //Verific daca pixelul se afla in una din zonele marginilor gridului
            int isLeftBorder   = (x >= (PLAYFIELD_OFFSET_H - TILE_SIZE) * PixelBytes && x < PLAYFIELD_OFFSET_H * PixelBytes);
            int isRightBorder  = (x >= (PLAYFIELD_OFFSET_H + PLAYFIELD_WIDTH) * PixelBytes && x < (PLAYFIELD_OFFSET_H + PLAYFIELD_WIDTH + TILE_SIZE) * PixelBytes);
            int isBottomBorder = (y >= PLAYFIELD_OFFSET_V + PLAYFIELD_HEIGHT && y < PLAYFIELD_OFFSET_V + PLAYFIELD_HEIGHT + TILE_SIZE);

            if (isLeftBorder || isRightBorder || isBottomBorder) {

                backBuffer[index + 1] =  color & 0xff; 		  // B
                backBuffer[index]     = (color >> 8) & 0xff;  // G
                backBuffer[index + 2] = (color >> 16) & 0xff; // R
            }
        }
    }
}

/* Functie care deseaneaza grafic ceea ce se afla stati in campul de
 * joc (fara piesa care este in cadere) */
void DrawPlayfield() {
	// Parcurgem matricea campului nostru care este vizibila de la randul 2
    for (int row = 2; row < GRID_M_ROWS; row++) {
        for (int col = 0; col < GRID_M_COLS; col++) {

            int val = playfield[row][col];
            // Luam culoarea asociata piesei din tabelul nostru
            u32 color = GetHexColor((char)val);
            // Daca nu este nimic atunci punem negru in rest
            if (val == 0) {
            	color = 0x000000;
            }

            // Setam coordonatele pixelilor de start al block-ului nostru
            // in cadrul campului de joc de pe ecran
            int yStart = PLAYFIELD_OFFSET_V + (row - 2) * TILE_SIZE;
            int xStart = PLAYFIELD_OFFSET_H + col * TILE_SIZE;

            // Se deseneaza blocul de 14x14 pixeli pe ecran (in campul de joc)
            for (int y = yStart; y < yStart + TILE_SIZE; y++) {
                for (int x = xStart * PixelBytes; x < (xStart + TILE_SIZE) * PixelBytes; x += PixelBytes) {

                    int index = y * FRAME_HORIZONTAL_SIZE * PixelBytes + x;

                    backBuffer[index + 1] = color & 0xff;         // Blue
                    backBuffer[index]     = (color >> 8) & 0xff;  // Green
                    backBuffer[index + 2] = (color >> 16) & 0xff; // Red
                }
            }
        }
    }
}

/* Functie care deseneaza pe ecran, piesa in pozitia ei noua simuland astfel
 * caderea ei prin resetarea campului grafic dupa fiecare desenare a piesei */
void DrawTetromino(){
	// Parcurgem matricea piesei tetromino
    for (int row = 0; row < TETROMINO_M_SIZE; row++){
        for (int col = 0; col < TETROMINO_M_SIZE; col++){
        	// Desenam daca pozitia curenta este pozitiva (este parte din piesa)
        	// si pozitia este vizibila grafic (peste primele 2 randuri)
            if(crtTetromino.matrix[row][col] && crtTetromino.row + row >= 2){
                u32 color = GetHexColor(crtTetromino.name);

                // Setam coordonatele pixelilor de start al block-ului nostru
                // in cadrul campului de joc vizibil de pe ecran
                int yStart = PLAYFIELD_OFFSET_V + (row + crtTetromino.row - 2) * TILE_SIZE;
                int xStart = PLAYFIELD_OFFSET_H + (col + crtTetromino.col) * TILE_SIZE;

                // Se deseneaza blocul de 14x14 pixeli pe ecran (in campul de joc)
                for (int y = yStart; y < yStart + TILE_SIZE; y++) {
                    for (int x = xStart * PixelBytes; x < (xStart + TILE_SIZE) * PixelBytes; x += PixelBytes) {

                        int index = y * FRAME_HORIZONTAL_SIZE * PixelBytes + x;

                        backBuffer[index + 1] = color & 0xff;         // Blue
                        backBuffer[index]     = (color >> 8) & 0xff;  // Green
                        backBuffer[index + 2] = (color >> 16) & 0xff; // Red
                    }
                }
            }
        }
    }
}

//Functie de test pentru miscarea unui patrat
void MovingSquare(int squareSize, int squarePosV, int *squarePosH, int *direction){
	usleep(750000); //Intarziere 1 sec pentru
	//Parcurgem cate o linie
	for(int i=0; i<FRAME_VERTICAL_SIZE; i++){
		//Parcurgem cate un pixel de 3 octeti, deasta e j+3
		for(int j=0; j<FRAME_HORIZONTAL_SIZE*PixelBytes; j=j+PixelBytes){

			int index = (i*FRAME_HORIZONTAL_SIZE*PixelBytes)+j;

			if(i >= squarePosV && i<= squarePosV + squareSize){
				if(j >= *squarePosH*PixelBytes && j<= *squarePosH*PixelBytes + squareSize*PixelBytes){
					//Scrie culoarea in buffer daca este zona patratului
					//backBuffer[index]   = 0xff; //G
					backBuffer[index+1] = 0xff; //B
					//backBuffer[index+2] = 0xff; //R

				}
				else{	//Goleste in rest unde nu este patratul
					backBuffer[index]   = 0x00; //G
					backBuffer[index+1] = 0x00; //B
					backBuffer[index+2] = 0x00; //R
				}
			}
			else{	//Goleste in rest unde nu este patratul
				backBuffer[index]   = 0x00; //G
				backBuffer[index+1] = 0x00; //B
				backBuffer[index+2] = 0x00; //R
			}

		}
	}

	if(*squarePosH >= (FRAME_HORIZONTAL_SIZE-squareSize)){
		*direction = 0;
	}else if(*squarePosH <= 0){
		*direction = 1;
	}

	if(*direction){
		(*squarePosH) += squareSize;
	}
	else{
		(*squarePosH) -= squareSize;
	}

}

/*============================ Logica Tetris ==============================*/

/* Functie care returneaza structura initializata a urmatoarei piese tetromino */
Tetromino GenerateNextTetromino(){
	Tetromino t;
	t.name = GetNextTetrName();
    //Initializam cu 0 matricea initiala
    memset(t.matrix, 0, sizeof(t.matrix));
    // Copiem matricea default a piesei în structura Tetromino
    memcpy(t.matrix, GetTetrominoMatrix(t.name), sizeof(t.matrix));     //int (*source)[TETROMINO_M_SIZE] = GetTetrominoMatrix(t.name);

    int length;
    // Centram piesele in spatiul de joc cu specificarea ca piesa O are o lungime
    // diferita si poate fi centrata diferit.
    if (t.name == 'O') {
        length = 2;
    } else { length = 4; }

    t.col = GRID_M_COLS / 2 - length / 2;

    //Piesa I incepe de pe randul 21 (1), restul incep de pe randul 22 (0)
    t.row = (t.name == 'I') ? 1 : 0;

    return t;
}

/* Functie pt amestecarea secventei numelor pieselor prin metoda Fisher Yates */
void ShuffleSequence(char* array, int size) {
    for (int i = size - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

/* Functie ce returneaza numele unei piesei tetromino din cele 7 posibile amestecate aleator*/
char GetNextTetrName() {
    if (sequenceIndex == 0 ) { //Daca s-a epuizat secventa se genereaza alta
        char sequence[NO_TETROMINOS] = {'I', 'J', 'L', 'O', 'S', 'T', 'Z'};

        ShuffleSequence(sequence, NO_TETROMINOS); //Amestecarea aleatoare a secventei

        for (int i = 0; i < NO_TETROMINOS; i++) {
            tetrominoSequence[i] = sequence[i];
        }
    }

    char next = tetrominoSequence[sequenceIndex];
    sequenceIndex++;

    if (sequenceIndex >= NO_TETROMINOS) {
        sequenceIndex = 0;
    }

    return next;
}

/* Functie ce primeste numele piesei si returneaza matricea default a acesteia */
void* GetTetrominoMatrix(char name){
    static int I[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {0, 0, 0, 0},
        {1, 1, 1, 1},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    static int J[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {1, 0, 0, 0},
        {1, 1, 1, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    static int L[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {0, 0, 1, 0},
        {1, 1, 1, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    static int O[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {1, 1, 0, 0},
        {1, 1, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    static int S[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {0, 1, 1, 0},
        {1, 1, 0, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    static int Z[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {1, 1, 0, 0},
        {0, 1, 1, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    static int T[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {
        {0, 1, 0, 0},
        {1, 1, 1, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0}
    };

    switch (name) {
        case 'I': return I;
        case 'J': return J;
        case 'L': return L;
        case 'O': return O;
        case 'S': return S;
        case 'Z': return Z;
        case 'T': return T;
        default: return 0;
    }
}

/* Seteaza un seed diferit la fiecare pornire*/
void SetRndSeed(){
    XTime t;
    XTime_GetTime(&t); // t este uint64_t
    u32 seed = (u32)(t & 0xFFFFFFFF);
    srand(seed);
}

/* Se realizeaza verificarea pozitiei pisei dupa modificarile de pozitie */
int ValidMove(int tRow, int tCol, int tMatrix[TETROMINO_M_SIZE][TETROMINO_M_SIZE]) {
    // Parcurgem matricea piesei curente pentru validarea pozitiei
    for (int row = 0; row < TETROMINO_M_SIZE; row++) {
        for (int col = 0; col < TETROMINO_M_SIZE; col++) {
            // Luam in considerare doar piesa (unde este 1)
            if (tMatrix[row][col]) {
                // Gasim pozitia din campul de joc a block-ului curent din piesa
                // (Un block reprezinta o bucata din piesa noastra)
                int blockRow = tRow + row;
                int blockCol = tCol + col;

                // Verificam daca piesa se suprapune cu marginile campului
                if (blockCol < 0 || blockCol >= GRID_M_COLS || blockRow >= GRID_M_ROWS) {
                    return 0;
                }
                // Verificam daca piesa se suprapune cu alta piesa deja plasata
                if (playfield[blockRow][blockCol]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

/* Se plaseaza piesa in camp dupa ce se verifica daca aceasta nu a depasit inaltimea campului
 * de joc. Dupa plasarea piesei in camp se apeleaza si functia care curata liniile pline din acesta. */
int PlaceTetromino() {
    for (int row = 0; row < TETROMINO_M_SIZE; row++) {
        for (int col = 0; col < TETROMINO_M_SIZE; col++) {
            if (crtTetromino.matrix[row][col]) {
                // Joc oprit daca piesa se afla inafara campului vizibil (inafara 2-22)
                if (crtTetromino.row + row < 2) {
                    return 1; // gameOver = 1;
                }
                // Punem piesa in camp prin numele ei pentru a putea fi diferentiate mai departe
                // Caracterul piesei (char) va fi convertit la int care va corespunde campului
                playfield[crtTetromino.row + row][crtTetromino.col + col] = crtTetromino.name;
            }
        }
    }
    // Verificam daca au fost umplute liniile dupa plasare
    CheckLines();

    crtTetromino = GenerateNextTetromino();

    return 0; // gameOver = 0;
}

/* Se realizeaza verificarea liniilor pline de jos in sus pentru a le elibera si
 * a aduna punctele la scor*/
void CheckLines() {
    int linesCleared = 0;
    // Incepem verificarea liniilor pline de jos in sus pentru eliberare
    for (int row = GRID_M_ROWS - 1; row >= 0; ) {
        int full = 1;
        // Se verifica daca linia este plina sau nu;
        for (int col = 0; col < GRID_M_COLS; col++) {
            if (playfield[row][col] == 0) {
                full = 0;
                break;
            }
        }
        // Linia plina se va elibera si se vor cobora cele de deasupra ei
        if (full) {
            linesCleared++;

            for (int r = row; r >= 0; r--) {
                for (int c = 0; c < GRID_M_COLS; c++) {
                    playfield[r][c] = playfield[r - 1][c];
                }
            }
        }
        else {
            row--;
        }
    }

    if(linesCleared){
    	totalLinesCleared += linesCleared;

    	UpdateLevel();

    	// Incrementarea scorului in functie de nr de linii curatate
    	// deodata si nivelul curent
    	gameScore += 50 * (2 * linesCleared - 1) * currentLevel;

    	ClearTerminal();
        //Afisez scorul si nivelul in terminal
    	xil_printf("\n\n\n\n\n\n\n\n\t\t\t\tScor: %d\r\n", gameScore);
    	xil_printf("\n\t\t\t\tNivel: %d\r\n", currentLevel);
    }

}

/*Functie care actualizeaza scorul atins si viteaza curenta*/
void UpdateLevel(){
	// La fiecare 5 linii curatate se ajunge la un nivel mai inalt
	if (totalLinesCleared >= LINES_THRESHOLD){
		totalLinesCleared -= LINES_THRESHOLD;

		currentLevel++;

		// Viteza se va incrementa pana la nivelul maxim de 10
		if(currentLevel <= MAX_LEVEL){
			gameSpeed -= SPEED_DECREASE;
		}
	}
}

/*Functie care realizeaza stergerea caracterelor din terminal si mutarea cursorului*/
void ClearTerminal(){
    xil_printf("\x1b[2J"); // Curat terminalul de afisare
    xil_printf("\x1b[H");  // Mutam cursorul la inceputul terminalului
}

/* Functie care realizeaza modificarile asupra piesei in functie de tasta apasata. */
void InputCotrol(){
	// Daca se primeste un vreun caracter prin intermediul UART se face controlul
	if (XUartPs_IsReceiveData(UART_BASEADDR)) {
		// Preluam caracterul si in functie de acesta se modifica piesa curenta
		char c = XUartPs_RecvByte(UART_BASEADDR);
		switch (c) {
			case 'w': // Rotire in sensul acelor de ceasornic
				RotateTetrominoMatrix();
				break;
			case 'a': // Mutare la stanga
				if (ValidMove(crtTetromino.row, crtTetromino.col - 1, crtTetromino.matrix)) {
				    crtTetromino.col--;
				}
				break;
			case 's': // Coborare rapida
				if(ValidMove(crtTetromino.row + 1, crtTetromino.col, crtTetromino.matrix)){
					crtTetromino.row++;
				}
				break;
			case 'd': // Mutare la dreapta
				if (ValidMove(crtTetromino.row, crtTetromino.col + 1, crtTetromino.matrix)) {
				    crtTetromino.col++;
				}
				break;
		}
	}

}

/* Functie care realizeaza rotirea matricii daca este posibil */
void RotateTetrominoMatrix(){
	// Piesa O nu se roteste
    if (crtTetromino.name == 'O') {
        return;
    }

    int rotated[TETROMINO_M_SIZE][TETROMINO_M_SIZE] = {0};

    // In cazul piesei I rotim toata matricea de 4x4
    if (crtTetromino.name == 'I') {
        for (int r = 0; r < TETROMINO_M_SIZE; r++) {
            for (int c = 0; c < TETROMINO_M_SIZE; c++) {
                rotated[c][TETROMINO_M_SIZE - 1 - r] = crtTetromino.matrix[r][c];
            }
        }
    // In cazul altor piese rotim doar matricea de 3x3 din stanga sus
    } else {
    	int secMatrixSize = 3;
        for (int r = 0; r < secMatrixSize; r++) {
            for (int c = 0; c < secMatrixSize; c++) {
                rotated[c][secMatrixSize - 1 - r] = crtTetromino.matrix[r][c];
            }
        }
    }
    //Copiaza matricea rotita in matricea originala
    if (ValidMove(crtTetromino.row, crtTetromino.col, rotated)) {
        memcpy(crtTetromino.matrix, rotated, sizeof(crtTetromino.matrix));
    }
}


/*============================ Initializare ==============================*/

// Functie pentru initializare/configurare VDMA si alte componente
void InitComponents(){
    int Status;
    XAxiVdma_Config *vdma_cfg; //Pointer de configurare VDMA
    XAxiVdma_FrameCounter FrameCfg; //Pointer de config a frame-urilor pt intrerupere

    //XGpio_Initialize(&LED, LED_ID);
    //Setez toate ledurile ca fiind iesire (0 = OUT, 1 = IN)
    //XGpio_SetDataDirection(&LED, LED_CHANNEL, 0b0000);

    //Setez toate butoanele ca fiind intrare
    XGpio_Initialize(&BTN, BTN_ID);
    XGpio_SetDataDirection(&BTN, BTN_CHANNEL, BTN_MASK); //(0 = OUT, 1 = IN)

    vdma_cfg = XAxiVdma_LookupConfig(VDMA_ID); //Pointer la configuratia hardware

    ReadCount = vdma_cfg->MaxFrameStoreNum; //Frame-buffere, numarul de frame-uri pe care lucram
    XAxiVdma_CfgInitialize(&VDMA, vdma_cfg, vdma_cfg->BaseAddress); //Initializare DMA
    //XAxiVdma_SetFrmStore(&VDMA, ReadCount, XAXIVDMA_READ); //Optional

	FrameCfg.ReadFrameCount = ReadCount;
	FrameCfg.ReadDelayTimerCount = DELAY_TIMER_COUNTER;
	XAxiVdma_SetFrameCounter(&VDMA, &FrameCfg);

	ReadSetup(&VDMA); //Setup-ul necesar citirii

	Status = SetupIntrSystem(&VDMA, READ_INTR_ID); //Setup-ul necesar intreruperilor
	if (Status != XST_SUCCESS) {
		xil_printf(
			"Setup interrupt system failed for read %d\r\n", Status);
	}
	//Activeaza intreruperile de completare de frame
	XAxiVdma_IntrEnable(&VDMA, XAXIVDMA_IXR_COMPLETION_MASK, XAXIVDMA_READ);

	//Pornim VDMA pentru citire din buffer
	XAxiVdma_DmaStart(&VDMA, XAXIVDMA_READ);
}

/*****************************************************************************/
/*This function sets up the read channel
*
* @param	InstancePtr is the instance pointer to the DMA engine.
*
* @note		Function made with help of Xilinx examples.
******************************************************************************/
void ReadSetup(XAxiVdma *InstancePtr)
{
	int Index;
	UINTPTR Addr = (u32)&(frontBuffer[0]);

	ReadCfg.VertSizeInput = FRAME_VERTICAL_LEN;
	ReadCfg.HoriSizeInput = FRAME_HORIZONTAL_LEN;
	ReadCfg.Stride = FRAME_HORIZONTAL_LEN;
	ReadCfg.FrameDelay = 0;
    ReadCfg.EnableCircularBuf = 0; // 1 Citire continua buffere vdma, 0 Parking pe un frame
    ReadCfg.EnableSync = 0;
    ReadCfg.PointNum = 0;
    ReadCfg.EnableFrameCounter = 0;
    ReadCfg.FixedFrameStoreAddr = 0;
    //Configurarea DMA-ului pentru setarile de citire aplicate mai sus
    XAxiVdma_DmaConfig(InstancePtr, XAXIVDMA_READ, &ReadCfg);

    //Initializarea adreselor pt frame-buffere (aici avem doar unul)
	for(Index = 0; Index < ReadCount; Index++) {
		ReadCfg.FrameStoreStartAddr[Index] = Addr;
		Addr +=  FRAME_SIZE;
	}

	XAxiVdma_DmaSetBufferAddr(&VDMA, XAXIVDMA_READ, ReadCfg.FrameStoreStartAddr);
}

/*****************************************************************************
* This function setups the interrupt system so interrupts can occur for the
* DMA.  This function assumes INTC component exists in the hardware system.
*
* @param	AxiDmaPtr is a pointer to the instance of the DMA engine
* @param	ReadIntrId is the read channel Interrupt ID.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note		Function made with help of Xilinx examples.
******************************************************************************/
static int SetupIntrSystem(XAxiVdma *AxiVdmaPtr, u16 ReadIntrId)
{
	int Status;
	XScuGic *IntcInstancePtr = &Intc;

	/* Initialize the interrupt controller and connect the ISRs */
	XScuGic_Config *IntcConfig;
	IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
	Status =  XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig, IntcConfig->CpuBaseAddress);
	if(Status != XST_SUCCESS){
		xil_printf("Interrupt controller initialization failed..");
		return -1;
	}

	Status = XScuGic_Connect(IntcInstancePtr,ReadIntrId,(Xil_InterruptHandler)XAxiVdma_ReadIntrHandler,(void *)AxiVdmaPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed read channel connect intc %d\r\n", Status);
		return XST_FAILURE;
	}

	XScuGic_Enable(IntcInstancePtr,ReadIntrId);

	Xil_ExceptionInit();
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,(Xil_ExceptionHandler)XScuGic_InterruptHandler,(void *)IntcInstancePtr);
	Xil_ExceptionEnable();

	/* Register call-back functions*/
	XAxiVdma_SetCallBack(AxiVdmaPtr, XAXIVDMA_HANDLER_GENERAL, ReadCallBack, (void *)AxiVdmaPtr, XAXIVDMA_READ);
	XAxiVdma_SetCallBack(AxiVdmaPtr, XAXIVDMA_HANDLER_ERROR, ReadErrorCallBack, (void *)AxiVdmaPtr, XAXIVDMA_READ);

	return XST_SUCCESS;
}

//Call back function for read channel interrupt
static void ReadCallBack()
{
	//xil_printf("Interrupt for frame end called\r\n");
	memcpy(frontBuffer, backBuffer, FRAME_SIZE);
	Xil_DCacheFlush((UINTPTR)frontBuffer, FRAME_SIZE); //Curatam memoria cache pentru a nu aparea artefacte pe ecran
}

//Call back function for read channel error interrupt
static void ReadErrorCallBack()
{
	xil_printf("Interrupt for read error called\r\n");
}
