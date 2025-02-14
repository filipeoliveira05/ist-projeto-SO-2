# SO 24/25 - Projeto 2
**Filipe Oliveira**  
**Número de estudante**: ist1110633

## 📌 Descrição do Projeto  

A segunda parte do projeto visa **expandir a funcionalidade do IST-KVS**, permitindo a **interação com processos clientes** e a **gestão de conexões** via **named pipes e sinais**.  

### 🔹 Objetivos principais:
- **Permitir que processos clientes monitorizem pares chave-valor** via **named pipes**.  
- **Gerir múltiplas sessões de clientes** simultaneamente, garantindo sincronização correta.  
- **Encerrar conexões de clientes usando sinais** (`SIGUSR1`).  

---

## 🎯 Funcionalidades Implementadas  

### 🔌 **Exercício 1: Comunicação Cliente-Servidor via Named Pipes**  
O IST-KVS deve passar a ser um **servidor autónomo**, iniciado com:  
```bash
./kvs <dir_jobs> <max_threads> <backups_max> <nome_do_FIFO_de_registo>
``` 

O servidor cria um **named pipe (FIFO)** para receber conexões de clientes.  

Os clientes conectam-se ao **IST-KVS** com o seguinte comando:  
```bash
./client <id_do_cliente> <nome_do_FIFO_de_registo>
```
Cada cliente cria **três FIFOs** para comunicação:  
- **FIFO de notificações**  
- **FIFO de pedidos**  
- **FIFO de respostas**  

Enquanto uma sessão está ativa, o cliente recebe **atualizações automáticas** das chaves subscritas.  

---  

## 🚀 Exercício 2: Gerenciamento de Sessões com Sinais  

- O servidor aceita até **S conexões concorrentes**.  
- Se um novo cliente tentar conectar-se quando o limite for atingido, ele **aguarda** uma sessão encerrar.  
- O servidor **detecta desconexões automaticamente** e limpa as subscrições do cliente.  
- O servidor escuta o **sinal `SIGUSR1`**, que **encerra todas as sessões ativas**, mas mantém o **IST-KVS** em execução.


---

📄 **Nota:** Para mais informações e explicações detalhadas, consultar [enunciado-SO2425P2.pdf](./enunciado-SO2425P2.pdf)