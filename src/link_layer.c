// Link layer protocol implementation

#include <unistd.h>
#include "link_layer.h"
#include "alarm.h"

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source

int alarmEnabled = FALSE;
int alarmCount = 1;
int senderNumber = 0, receiverNumber = 1;
int nTries, timeout, fd, lastFrameNumber = -1;

void config(LinkLayer connectionParameters) {
    fd = open(connectionParameters.serialPort, O_RDWR | O_NOCTTY | O_NONBLOCK);

    alarmCount = 0;

    if (fd < 0) {
        perror(connectionParameters.serialPort);
        exit(-1);
    }

    struct termios oldtio;
    struct termios newtio;

    // Save current port settings
    if (tcgetattr(fd, &oldtio) == -1) {
        perror("tcgetattr");
        exit(-1);
    }

    // Clear struct for new port settings
    memset(&newtio, 0, sizeof(newtio));

    newtio.c_cflag = connectionParameters.baudRate | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;

    // Set input mode (non-canonical, no echo,...)
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0; // Inter-character timer unused
    newtio.c_cc[VMIN] = 1;  // Blocking read until 5 chars received

    // VTIME e VMIN should be changed in order to protect with a
    // timeout the reception of the following character(s)

    // Now clean the line and activate the settings for the port
    // tcflush() discards data written to the object referred to
    // by fd but not transmitted, or data received but not read,
    // depending on the value of queue_selector:
    //   TCIFLUSH - flushes data received but not read.
    tcflush(fd, TCIOFLUSH);

    // Set new port settings
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");
}

int sendSupervisionFrame(int fd, unsigned char A, unsigned char C) {
    unsigned char FRAME[5] = {FLAG, A, C, A ^ C, FLAG};
    return write(fd, FRAME, 5);
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters) {
    config(connectionParameters);
    printf("\n------------------------------LLOPEN------------------------------\n\n");

    nTries = connectionParameters.nRetransmissions;
    timeout = connectionParameters.timeout;

    if (connectionParameters.role == LlTx) {
        unsigned char buf[5] = {0}, parcels[5] = {0};

        buf[0] = 0x7E;
        buf[1] = 0x03;
        buf[2] = 0x03;
        buf[3] = 0x00;
        buf[4] = 0x7E;

        while (alarmCount < nTries) {
            if (!alarmEnabled) {
                int bytes = write(fd, buf, sizeof(buf));
                printf("\nSET message sent, %d bytes written\n", bytes);
                startAlarm(timeout);
            }

            int result = read(fd, parcels, 5);
            if (result != -1 && parcels != 0 && parcels[0] == 0x7E) {
                //se o UA estiver errado 
                if (parcels[2] != 0x07 || (parcels[3] != (parcels[1] ^ parcels[2]))) {
                    printf("\nUA not correct: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2], parcels[3],
                           parcels[4]);
                    alarmEnabled = FALSE;
                    continue;
                } else {
                    printf("\nUA correctly received: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2],
                           parcels[3], parcels[4]);
                    alarmEnabled = FALSE;
                    break;
                }
            }
        }

        if (alarmCount >= nTries) {
            printf("\nAlarm limit reached, SET message not sent\n");
            return -1;
        }
    } else {
        unsigned char parcels[5] = {0}; // +1: Save space for the final '\0' char

        LinkLayerState state = START;
        unsigned char byte;

        switch (connectionParameters.role) {
            case LlTx: {
                (void) signal(SIGALRM, alarmHandler);
                while (connectionParameters.nRetransmissions != 0 && state != STOP_R) {

                    sendSupervisionFrame(fd, A_ER, C_SET);
                    alarm(connectionParameters.timeout);
                    alarmEnabled = FALSE;

                    while (alarmEnabled == FALSE && state != STOP_R) {
                        if (read(fd, &byte, 1) > 0) {
                            switch (state) {
                                case START:
                                    if (byte == FLAG) state = FLAG_RCV;
                                    break;
                                case FLAG_RCV:
                                    if (byte == A_RE) state = A_RCV;
                                    else if (byte != FLAG) state = START;
                                    break;
                                case A_RCV:
                                    if (byte == C_UA) state = C_RCV;
                                    else if (byte == FLAG) state = FLAG_RCV;
                                    else state = START;
                                    break;
                                case C_RCV:
                                    if (byte == (A_RE ^ C_UA)) state = BCC1_OK;
                                    else if (byte == FLAG) state = FLAG_RCV;
                                    else state = START;
                                    break;
                                case BCC1_OK:
                                    if (byte == FLAG) state = STOP_R;
                                    else state = START;
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                    connectionParameters.nRetransmissions--;
                }
                if (state != STOP_R) return -1;
                break;
            }

            case LlRx: {
                while (state != STOP_R) {
                    if (read(fd, &byte, 1) > 0) {
                        switch (state) {
                            case START:
                                if (byte == FLAG) state = FLAG_RCV;
                                break;
                            case FLAG_RCV:
                                if (byte == A_ER) state = A_RCV;
                                else if (byte != FLAG) state = START;
                                break;
                            case A_RCV:
                                if (byte == C_SET) state = C_RCV;
                                else if (byte == FLAG) state = FLAG_RCV;
                                else state = START;
                                break;
                            case C_RCV:
                                if (byte == (A_ER ^ C_SET)) state = BCC1_OK;
                                else if (byte == FLAG) state = FLAG_RCV;
                                else state = START;
                                break;
                            case BCC1_OK:
                                if (byte == FLAG) state = STOP_R;
                                else state = START;
                                break;
                            default:
                                break;
                        }
                    }
                }
                sendSupervisionFrame(fd, A_RE, C_UA);
                break;
            }
            default:
                return -1;
        }

        parcels[2] = 0x07;
        parcels[3] = parcels[1] ^ parcels[2];

        int bytes = write(fd, parcels, sizeof(parcels));
        printf("UA message sent, %d bytes written\n", bytes);

        return fd;
    }

    alarmEnabled = FALSE;

    /*if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
    {
        perror("tcsetattr");
        exit(-1);
    }*/

    //close(fd);

    return 1;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(const unsigned char *buf, int bufSize) {

    //1º criar o BCC para o dataPacket
    //2º fazer byte stuffing
    //3º criar a nova infoFrame com o dataPacket (ja stuffed) la dentro
    //4º enviar a infoFrame e contar o alarme
    //5º factCheck a frame recebida do llread (ver se tem erros ou assim)
    //6º llwrite so termina quando recebe mensagem de sucesso ou quando o limite de tentativas é excedido

    printf("\n------------------------------LLWRITE------------------------------\n\n");

    alarmCount = 0;

    unsigned char BCC = 0x00, infoFrame[600] = {0}, parcels[5] = {0};
    int index = 4, STOP = 0, controlReceiver = (!senderNumber << 7) | 0x05;

    //BCC working correctly
    for (int i = 0; i < bufSize; i++) {
        BCC = (BCC ^ buf[i]);
    }

    infoFrame[0] = 0x7E; //Flag
    infoFrame[1] = 0x03; //Address
    infoFrame[2] = (senderNumber << 6); //Control
    infoFrame[3] = infoFrame[1] ^ infoFrame[2];

    for (int i = 0; i < bufSize; i++) {
        if (buf[i] == 0x7E) {
            infoFrame[index++] = 0x7D;
            infoFrame[index++] = 0x5e;
            continue;
        } else if (buf[i] == 0x7D) {
            infoFrame[index++] = 0x7D;
            infoFrame[index++] = 0x5D;
            continue;
        }

        infoFrame[index++] = buf[i];
    }

    if (BCC == 0x7E) {
        infoFrame[index++] = 0x7D;
        infoFrame[index++] = 0x5e;
    } else if (BCC == 0x7D) {
        infoFrame[index++] = 0x7D;
        infoFrame[index++] = 0x5D;
    } else { infoFrame[index++] = BCC; }

    infoFrame[index++] = 0x7E;

    while (!STOP) {
        if (!alarmEnabled) {
            write(fd, infoFrame, index);
            printf("\nInfoFrame sent NS=%d\n", senderNumber);
            startAlarm(timeout);
        }

        int result = read(fd, parcels, 5);

        if (result != -1 && parcels != 0) {
            if (parcels[2] != (controlReceiver) || (parcels[3] != (parcels[1] ^ parcels[2]))) {
                printf("\nRR not correct: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2], parcels[3],
                       parcels[4]);
                alarmEnabled = FALSE;
                continue;
            } else {
                printf("\nRR correctly received: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2],
                       parcels[3], parcels[4]);
                alarmEnabled = FALSE;
                STOP = 1;
            }
        }

        if (alarmCount >= nTries) {
            printf("\nllwrite error: Exceeded number of tries when sending frame\n");
            STOP = 1;
            close(fd);
            return -1;
        }
    }

    if (senderNumber) {
        senderNumber = 0;
    } else { senderNumber = 1; }

    return 0;
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(unsigned char *packet, int *sizeOfPacket) {
    printf("\n------------------------------LLREAD------------------------------\n\n");

    unsigned char infoFrame[600] = {0}, supFrame[5] = {0}, BCC2 = 0x00, aux[400] = {0}, STOP = FALSE;
    int control = (!receiverNumber) << 6, index = 0, sizeInfo = 0;

    unsigned char buf[1] = {0}; // +1: Save space for the final '\0' char

    LinkLayerState state = START;
    unsigned char readByte = TRUE;

    // Loop for input
    while (!STOP) {
        if (readByte) {
            int bytes = read(fd, buf, 1); //ler byte a byte
            if (bytes == -1 || bytes == 0)
                continue; // se der erro a leitura ou se tiver lido 0 bytes continuo para a próxima iteraçao

        }

        switch (state) {
            case START:
                if (buf[0] == 0x7E) {
                    state = FLAG_RCV;
                    infoFrame[sizeInfo++] = buf[0];
                }
                break;

            case FLAG_RCV:
                if (buf[0] != 0x7E) {
                    state = A_RCV;
                    infoFrame[sizeInfo++] = buf[0];
                } else {
                    memset(infoFrame, 0, 600);
                    state = FLAG_RCV;
                    sizeInfo = 0;
                    infoFrame[sizeInfo++] = buf[0];
                }
                break;

            case A_RCV:
                if (buf[0] != 0x7E) {
                    infoFrame[sizeInfo++] = buf[0];
                } else if (buf[0] == 0x7E) {
                    STOP = TRUE;
                    infoFrame[sizeInfo++] = buf[0];
                    readByte = FALSE;
                }
                break;

            default:
                break;
        }
    }

    //1º ler o pipe
    //2º fazer de-stuff aos bytes lidos
    //3º verificar que os BCCs estao certos
    //4º enviar a mensagem de confirmacao de receçao, positiva se correu tudo bem, negativa se BCC ou algo correu mal

    supFrame[0] = 0x7E;
    supFrame[1] = 0x03;

    supFrame[4] = 0x7E;

    if ((infoFrame[1] ^ infoFrame[2]) != infoFrame[3] || infoFrame[2] != control) {
        printf("\nInfoFrame not received correctly. Protocol error. Sending REJ.\n");
        supFrame[2] = (receiverNumber << 7) | 0x01;
        supFrame[3] = supFrame[1] ^ supFrame[2];
        write(fd, supFrame, 5);

        printf("\n-----REJ-----\n");
        printf("\nSize of REJ: %d\nREJ: 0x", 5);

        for (int i = 0; i < 5; i++) {
            printf("%02X ", supFrame[i]);
        }

        printf("\n\n");

        return -1;
    }

    for (int i = 0; i < sizeInfo; i++) {
        if (infoFrame[i] == 0x7D && infoFrame[i + 1] == 0x5e) {
            packet[index++] = 0x7E;
            i++;
        } else if (infoFrame[i] == 0x7D && infoFrame[i + 1] == 0x5d) {
            packet[index++] = 0x7D;
            i++;
        } else { packet[index++] = infoFrame[i]; }
    }

    int size = 0;

    if (packet[4] == 0x01) {
        size = 256 * packet[6] + packet[7] + 4 + 6; //+4 para contar com os bytes de controlo, numero de seq e tamanho
        for (int i = 4; i < size - 2; i++) {
            BCC2 = BCC2 ^ packet[i];
        }
    } else {
        size += packet[6] + 3 +
                4; //+3 para contar com os bytes de C, T1 e L1 // +4 para contar com os bytes FLAG, A, C, BCC
        size += packet[size + 1] + 2 + 2; //+2 para contar com T2 e L2 //+2 para contar com BCC2 e FLAG

        for (int i = 4; i < size - 2; i++) {
            BCC2 = BCC2 ^ packet[i];
        }
    }

    if (packet[size - 2] == BCC2) {
        if (packet[4] == 0x01) {
            if (infoFrame[5] == lastFrameNumber) {
                printf("\nInfoFrame received correctly. Repeated Frame. Sending RR.\n");
                supFrame[2] = (receiverNumber << 7) | 0x05;
                supFrame[3] = supFrame[1] ^ supFrame[2];
                write(fd, supFrame, 5);
                return -1;
            } else {
                lastFrameNumber = infoFrame[5];
            }
        }
        printf("\nInfoFrame received correctly. Sending RR.\n");
        supFrame[2] = (receiverNumber << 7) | 0x05;
        supFrame[3] = supFrame[1] ^ supFrame[2];
        write(fd, supFrame, 5);
    } else {
        printf("\nInfoFrame not received correctly. Error in data packet. Sending REJ.\n");
        supFrame[2] = (receiverNumber << 7) | 0x01;
        supFrame[3] = supFrame[1] ^ supFrame[2];
        write(fd, supFrame, 5);

        return -1;
    }

    (*sizeOfPacket) = size;

    index = 0;

    for (int i = 4; i < (*sizeOfPacket) - 2; i++) {
        aux[index++] = packet[i];
    }

    (*sizeOfPacket) = size - 6;

    memset(packet, 0, sizeof(packet));

    for (int i = 0; i < (*sizeOfPacket); i++) {
        packet[i] = aux[i];
    }

    if (receiverNumber) {
        receiverNumber = 0;
    } else { receiverNumber = 1; }

    return 1;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(int showStatistics, LinkLayer connectionParameters, float runTime) {
    alarmCount = 0;

    printf("\n------------------------------LLCLOSE------------------------------\n\n");

    if (connectionParameters.role == LlRx) {

        unsigned char buf[6] = {0}, parcels[6] = {0};
        unsigned char STOP = 0, UA = 0;

        buf[0] = 0x7E;
        buf[1] = 0x03;
        buf[2] = 0x0B;
        buf[3] = buf[1] ^ buf[2];
        buf[4] = 0x7E;
        buf[5] = '\0';

        while (!STOP) {
            int result = read(fd, buf, 5);

            parcels[5] = '\0';

            if (result == -1) {
                continue;
            } else if (strcasecmp(buf, parcels) == 0) {
                printf("\nDISC message received. Responding now.\n");

                buf[1] = 0x01;
                buf[3] = buf[1] ^ buf[2];

                while (alarmCount < nTries) {
                    if (!alarmEnabled) {
                        printf("\nDISC message sent, %d bytes written\n", 5);
                        write(fd, buf, 5);
                        startAlarm(timeout);
                    }

                    int result = read(fd, buf, 5);
                    if (result != -1 && buf != 0 && buf[0] == 0x7E) {
                        //se o UA estiver errado
                        if (parcels[2] != 0x07 || (parcels[3] != (parcels[1] ^ parcels[2]))) {
                            printf("\nUA not correct: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2],
                                   parcels[3], parcels[4]);
                            alarmEnabled = FALSE;
                            continue;
                        } else {
                            printf("\nUA correctly received: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1],
                                   parcels[2], parcels[3], parcels[4]);
                            alarmEnabled = FALSE;
                            close(fd);
                            break;
                        }
                    }
                }

                if (alarmCount >= nTries) {
                    printf("\nAlarm limit reached, DISC message not sent\n");
                    return -1;
                }
                STOP = TRUE;
            }
        }
    } else {
        alarmCount = 0;

        unsigned char buf[6] = {0}, parcels[6] = {0};

        buf[0] = 0x7E;
        buf[1] = 0x03;
        buf[2] = 0x0B;
        buf[3] = buf[1] ^ buf[2];
        buf[4] = 0x7E;
        buf[5] = '\0';

        while (alarmCount < nTries) {
            if (!alarmEnabled) {
                int bytes = write(fd, buf, 5);
                printf("\nDISC message sent, %d bytes written\n", bytes);
                startAlarm(timeout);
            }

            int result = read(fd, buf, 5);

            buf[1] = 0x01;
            buf[3] = buf[1] ^ buf[2];
            parcels[5] = '\0';

            if (result != -1 && buf != 0 && buf[0] == 0x7E) {
                //se o DISC estiver errado 
                if (strcasecmp(buf, parcels) != 0) {
                    printf("\nDISC not correct: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2],
                           parcels[3], parcels[4]);
                    alarmEnabled = FALSE;
                    continue;
                } else {
                    printf("\nDISC correctly received: 0x%02x%02x%02x%02x%02x\n", parcels[0], parcels[1], parcels[2],
                           parcels[3], parcels[4]);
                    alarmEnabled = FALSE;

                    buf[1] = 0x01;
                    buf[2] = 0x07;
                    buf[3] = buf[1] ^ buf[2];

                    int bytes = write(fd, buf, 5);

                    close(fd);

                    printf("\nUA message sent, %d bytes written.\n\nI'm shutting off now, bye bye!\n", bytes);
                    return 1;
                }
            }
        }

        if (alarmCount >= nTries) {
            printf("\nAlarm limit reached, DISC message not sent\n");
            close(fd);
            return -1;
        }
    }

    if (showStatistics) {
        printf("\n------------------------------STATISTICS------------------------------\n\n");
        printf("\nNumber of packets sent: %d\nSize of data packets in information frame: %d\nTotal run time: %f\nAverage time per packet: %f\n",
               lastFrameNumber, 200, runTime, runTime / 200.0);
    }

    return 1;
}
