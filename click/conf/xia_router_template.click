elementclass Destination {
    $name |
    input -> Print("packet received by $name") -> XIAPrint(HLIM true) -> output;
};

elementclass SrcTypeCIDPreRouteProc {
    // input: a packet
    // output: a packet (passthru)

    input -> dup :: Tee(2);

    dup[0] -> output;
    dup[1] -> store_to_cache :: Discard;    // TODO : insert into cache and update route
};

elementclass GenericRouting4Port {
    // input: a packet to route
    // output[0]: forward to port 0~3 (painted)
    // output[1]: need to update "last" pointer
    // output[2]: no match

    input -> rt :: XIAXIDRouteTable;
    rt[0] -> Paint(0) -> [0]output;
    rt[1] -> Paint(1) -> [0]output;
    rt[2] -> Paint(2) -> [0]output;
    rt[3] -> Paint(3) -> [0]output;
    rt[4] -> [1]output;
    rt[5] -> [2]output;
};

elementclass GenericPostRouteProc {
    input -> XIADecHLIM -> output;
}

elementclass XIAPacketRoute {
    $name |
    // input: a packet to process
    // output[0]: forward (painted)
    // output[1]: arrived at destination node
    // output[2]: could not route at all (tried all paths)

    check_dest :: XIACheckDest();
    consider_first_path :: XIASelectPath(first);
    consider_next_path :: XIASelectPath(next);
    c :: XIAXIDTypeClassifier(next AD, next HID, next SID, next CID, -);

    //input -> Print("packet received by $name") -> check_dest;
    input -> check_dest;

    check_dest[0] -> [1]output; // arrived at the final destination
    check_dest[1] -> consider_first_path;

    consider_first_path[0] -> c;
    consider_first_path[1] -> [2]output;
    consider_next_path[0] -> c;
    consider_next_path[1] -> [2]output;

    //  Next destination is AD
    c[0] -> rt_AD :: GenericRouting4Port;
    rt_AD[0] -> GenericPostRouteProc -> [0]output;
    rt_AD[1] -> XIANextHop -> check_dest;
    rt_AD[2] -> consider_next_path;

    //  Next destination is HID
    c[1] -> rt_HID :: GenericRouting4Port;
    rt_HID[0] -> GenericPostRouteProc -> [0]output;
    rt_HID[1] -> XIANextHop -> check_dest;
    rt_HID[2] -> consider_next_path;

    //  Next destination is SID
    c[2] -> rt_SID :: GenericRouting4Port;
    rt_SID[0] -> GenericPostRouteProc -> [0]output;
    rt_SID[1] -> XIANextHop -> check_dest;
    rt_SID[2] -> consider_next_path;


    // change this if you want to do CID post route processing for any reason
    CIDPostRouteProc :: Null ; 

    //  Next destination is CID
    c[3] -> rt_CID :: GenericRouting4Port;
    rt_CID[0] -> GenericPostRouteProc -> CIDPostRouteProc -> [0]output;
    rt_CID[1] -> XIANextHop -> check_dest;
    rt_CID[2] -> consider_next_path;

    c[4] -> [2]output;
};

elementclass RouteEngine {
    $name |
    // input: a packet arrived at a node 
    // output[0]: forward (painted)
    // output[1]: arrived at destination node

    srcTypeClassifier :: XIAXIDTypeClassifier(src CID, -);
    proc :: XIAPacketRoute($name);

    input -> srcTypeClassifier[0] -> SrcTypeCIDPreRouteProc -> proc;
    srcTypeClassifier[1] -> proc;

    proc[0] -> [0]output; // Forward to other interface
    proc[1] -> [1]output; // Travel up the stack
    proc[2] -> Discard;  // No route drop (future TODO: return an error packet)
};

// 1-port host node
elementclass Host {
    $hid |

    // input: a packet arrived at a node 
    // output[0]: forward to port 0
    // output[1]: to destination handler

    n :: RouteEngine($hid);

    Script(write n/proc/rt_AD/rt.add - 0);      // default route for AD
    Script(write n/proc/rt_HID/rt.add - 0);     // default route for HID
    Script(write n/proc/rt_HID/rt.add $hid 4);  // self HID as destination
    Script(write n/proc/rt_SID/rt.add - 5);     // no default route for SID; consider other path
    Script(write n/proc/rt_CID/rt.add - 5);     // no default route for CID; consider other path

    input[0] -> n;
    input[1] -> n;
    n[0] -> Queue(200) -> [0]output;
    n[1] -> [1]output;
};

// 2-port router node
elementclass Router {
    $ad, $hid |

    // input: a packet arrived at a node 
    // output[0]: forward to port 0 (for $hid)
    // output[1]: forward to port 1 (for other ads)

    n :: RouteEngine($ad);
    
    Script(write n/proc/rt_AD/rt.add - 1);      // default route for AD
    Script(write n/proc/rt_AD/rt.add $ad 4);    // self AD as destination
    Script(write n/proc/rt_HID/rt.add $hid 0);  // forwarding for local HID
    Script(write n/proc/rt_SID/rt.add - 5);     // no default route for SID; consider other path
    Script(write n/proc/rt_CID/rt.add - 5);     // no default route for CID; consider other path

    input[0] -> n;
    input[1] -> n;
    n[0] -> sw :: PaintSwitch
    sw[0] -> Queue(200) -> [0]output;
    sw[1] -> Queue(200) -> [1]output;
    n[1] -> Discard;
};

elementclass RPC {
    $port, $isClient|

    sock::Socket(TCP, 0.0.0.0,$port, CLIENT $isClient);

    r::rpc();

    //localHost_out:: Print(CONTENTS 'ASCII') -> Discard;  //Replace Print for HID0
    //localHost_in0::TimedSource(INTERVAL 10, DATA "Application0 Request Served!") //Replace TimeSource for HID0
    //localHost_in1::TimedSource(INTERVAL 10, DATA "Application1 Request Served!") //Replace TimeSource for HID0
    //localHost_in2::TimedSource(INTERVAL 10, DATA "Application2 Request Served!") //Replace TimeSource for HID0

    sock -> [1] r;
    r[1] -> sock;

    r[0] -> output;
    input -> [0] r;
};

