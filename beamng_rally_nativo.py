"""Visor BeamNG nativo: recebe pontos via UDP, sem capturar o monitor.
python beamng_rally_nativo.py --porta COM6
python beamng_rally_nativo.py --porta COM6 --demo
"""
import argparse
import math
import time
import serial
from beamng_serial import AvisoTroca
from beamng_dados import FonteOutGauge, mensagem_painel
from beamng_mapa_nativo import FonteNavegacao, renderizar, demonstracao
from visor_serial import abrir_serial, ler_pedido, enquadrar, jpeg


def main():
    p=argparse.ArgumentParser(description='Painel e mapa nativo do BeamNG sem captura de tela')
    p.add_argument('--porta',default='COM6')
    p.add_argument('--udp-porta',type=int,default=4444)
    p.add_argument('--nav-porta',type=int,default=4445)
    p.add_argument('--alcance',type=float,default=400,help='Altura aproximada da area visivel, em metros')
    p.add_argument('--demo',action='store_true',help='Rota ficticia identificada como demonstracao; nao exige jogo')
    p.add_argument('--diagnostico',action='store_true',help='Mostra navegacao no terminal sem abrir USB')
    p.add_argument('--qualidade',type=int,default=65)
    p.add_argument('--shift-rpm',type=int)
    p.add_argument('--rpm-max',type=int,default=8000)
    p.add_argument('--turbo-max',type=float,default=2)
    args=p.parse_args()
    limite=args.shift_rpm if args.shift_rpm is not None else 6500
    if not 1000<=limite<=args.rpm_max<=30000: p.error('RPM fora da faixa')
    if not math.isfinite(args.alcance) or not 50<=args.alcance<=1500: p.error('Alcance deve ser 50..1500 m')
    if not 20<=args.qualidade<=90: p.error('Qualidade deve ser 20..90')
    if not math.isfinite(args.turbo_max) or not .1<=args.turbo_max<=100: p.error('Turbo-max invalido')
    if not (1<=args.udp_porta<=65535 and 1<=args.nav_porta<=65535) or args.udp_porta==args.nav_porta:
        p.error('Use portas UDP distintas entre 1 e 65535')
    carro=nav=None
    try:
        if args.diagnostico:
            nav=FonteNavegacao(args.nav_porta)
            print('Aguardando o mod nativo. Ctrl+C encerra.')
            while True:
                d=nav.ler()
                print('Sem dados do mod' if d is None else
                      f"Estado={d['status']} estradas={len(d.get('roads',[]))} pontos_rota={len(d.get('route',[]))}")
                time.sleep(1)
        carro=FonteOutGauge(args.udp_porta)
        if not args.demo: nav=FonteNavegacao(args.nav_porta)
        print('Modo DEMONSTRACAO: rota ficticia.' if args.demo else
              f'Mapa nativo: UDP local {args.nav_porta}. Ative o mod e defina um destino no jogo.')
        print(f'OutGauge: 127.0.0.1:{args.udp_porta}. Este programa nao captura o monitor.')
        print('Arraste para a esquerda para abrir o mapa. Ctrl+C encerra.')
        aviso=AvisoTroca(args.shift_rpm)
        with abrir_serial(args.porta) as porta:
            inicio=time.monotonic(); recebeu=False; alertou=False; ultima_pagina=None; ultimo_estado=None
            while True:
                linha=porta.readline()
                pagina=ler_pedido(linha)
                if pagina is None:
                    if not recebeu and not alertou and time.monotonic()-inicio>10:
                        print('Sem pedidos: confira o firmware beamng_rally e pressione EN se necessario.')
                        alertou=True
                    continue
                recebeu=True
                if pagina!=ultima_pagina:
                    print('Pagina:',pagina); ultima_pagina=pagina
                try:
                    if pagina==1:
                        tipo=b'T'
                        dados=carro.ler()
                        conteudo=mensagem_painel(dados,aviso,args.rpm_max,limite,args.turbo_max)
                        if conteudo is None:
                            aviso.ativo=False
                            tipo,conteudo=b'E',b'Sem OutGauge. Deslize para o mapa.'
                    else:
                        d=demonstracao(time.monotonic()-inicio) if args.demo else nav.ler()
                        estado=('sem_mod' if d is None else d['status'],len(d.get('route',[]))>1 if d else False)
                        if estado!=ultimo_estado:
                            print('Mapa:',estado[0],'/ com rota' if estado[1] else '/ sem rota')
                            ultimo_estado=estado
                        tipo,conteudo=b'J',jpeg(renderizar(d,args.alcance),args.qualidade)
                    pacote=enquadrar(tipo,conteudo)
                except (ValueError,KeyError,TypeError) as exc:
                    print('Dados rejeitados:',exc)
                    pacote=enquadrar(b'E',b'Confira o programa no PC')
                porta.write(pacote); porta.flush()
    except KeyboardInterrupt:
        print('\nEncerrado.')
    except (serial.SerialException,OSError) as exc:
        raise SystemExit(f'Erro: {exc}\nFeche os outros programas da COM6 e das portas UDP.') from exc
    finally:
        if carro: carro.fechar()
        if nav: nav.fechar()


if __name__=='__main__':
    main()
