package rubiks.ipl;

import ibis.ipl.*;
import java.util.ArrayList;
import java.util.Arrays;
import java.io.IOException;


/**
 * Solver for rubik's cube puzzle.
 *
 * @author Niels Drost, Timo van Kessel
 *
 */
public class Rubiks implements MessageUpcall  {

    static PortType portTypeMto1 = new PortType(PortType.COMMUNICATION_RELIABLE,
            PortType.SERIALIZATION_OBJECT, PortType.RECEIVE_EXPLICIT,
            PortType.CONNECTION_MANY_TO_ONE, PortType.RECEIVE_POLL);

    static PortType portType1to1 = new PortType(PortType.COMMUNICATION_RELIABLE,
            PortType.SERIALIZATION_OBJECT, PortType.RECEIVE_EXPLICIT,
            PortType.CONNECTION_ONE_TO_ONE);

    static PortType portTypeMto1Up = new PortType(PortType.COMMUNICATION_RELIABLE,
            PortType.SERIALIZATION_OBJECT, PortType.CONNECTION_MANY_TO_ONE, PortType.RECEIVE_AUTO_UPCALLS);

    static IbisCapabilities ibisCapabilities = new IbisCapabilities(
        IbisCapabilities.ELECTIONS_STRICT, IbisCapabilities.MEMBERSHIP_TOTALLY_ORDERED);

    static ArrayList<Cube> toDo = new ArrayList<Cube>();
    static Object lock = new Object();

    static IbisIdentifier[] joinedIbises;
    static int myIntIbisId;
    static int nodes;
    static Ibis myIbis;

    static ReceivePort workRequestReceiver;
    static SendPort workRequestSender;
    static ReceivePort workReceiver;
    //workSender create on demand
    static SendPort resultsSender;
    static ReceiverPort resultsReceiver;
    static SendPort terminationSender;
    static ReceivePort terminationReceiver;

    static SyncTermination syncTermination;
    static int requestsForWork=0;
    static int valuatedCubes=0;

    static IbisIdentifier server;


    static String[] arguments;


    public static int solution(Cube cube, CubeCache cache) {
        valuatedCubes++;
        //System.out.println("Ibis[" + myIntIbisId + "] -> solution");
        if (cube.isSolved()) {
            return 1;
        }

        if (cube.getTwists() >= cube.getBound()) {
            return 0;
        }
        //generate childrens
        Cube[] children = cube.generateChildren(cache);
        Cube child;
        int i;
        //add childrens on the toDo pool
        synchronized(lock) {
            for(i = 0; i < children.length; i++) {
                child = children[(children.length - 1) - i];
                toDo.add(child);
                //cache.put(child);
            }
        }
        return 0;
    }

    public static final boolean PRINT_SOLUTION = false;



    public static void printUsage() {
        System.out.println("Rubiks Cube solver");
        System.out.println("");
        System.out
        .println("Does a number of random twists, then solves the rubiks cube with a simple");
        System.out
        .println(" brute-force approach. Can also take a file as input");
        System.out.println("");
        System.out.println("USAGE: Rubiks [OPTIONS]");
        System.out.println("");
        System.out.println("Options:");
        System.out.println("--size SIZE\t\tSize of cube (default: 3)");
        System.out
        .println("--twists TWISTS\t\tNumber of random twists (default: 11)");
        System.out
        .println("--seed SEED\t\tSeed of random generator (default: 0");
        System.out
        .println("--threads THREADS\t\tNumber of threads to use (default: 1, other values not supported by sequential version)");
        System.out.println("");
        System.out
        .println("--file FILE_NAME\t\tLoad cube from given file instead of generating it");
        System.out.println("");
    }

    //method called to ask work to the server (local work pool empty)
    public static Cube askForWork() throws IOException, ClassNotFoundException {
        //System.out.println("Ibis[" + myIntIbisId + "] -> askForWork");
        Cube receivedWork = null;
        requestsForWork++;

        //send local work receiving port
        WriteMessage task = workRequestSender.newMessage();
        task.writeObject(workReceiver);
        task.finish();

        //get the work
        ReadMessage r = workReceiver.receive();
        receivedWork = (Cube)r.readObject();
        r.finish();

        //workRequestSender.disconnect(doner, "WorkReq");
        return receivedWork;
    }

    //extract the last element of the work pool and return it, null if the work pool is empty
    //method calle directly by the server and indirectly (through getWork()) by the Slaves
    synchronized public static Cube getFromPool (boolean sameNode) {
        Cube cube = null;
        synchronized(lock) {
            if(toDo.size() != 0) {
                int index;
                if(sameNode) {
                    index = toDo.size() - 1;
                } else {
                    index = 0;
                }
                cube = toDo.remove(index);
                //toDoWeight -= (c.getBound() - c.getTwists());
            }
        }
        return cube;
    }

    //method called when a new work request comes from a Slave
    public void upcall(ReadMessage message) throws IOException,
        ClassNotFoundException {
        ReceivePortIdentifier requestor = (ReceivePortIdentifier) message
                                          .readObject();

        System.err.println("received request from: " + requestor);

        // finish the request message. This MUST be done before sending
        // the reply message. It ALSO means Ibis may now call this upcall
        // method agian with the next request message
        message.finish();

        // create a sendport for the reply
        SendPort workSender = myIbis.createSendPort(portTypeMto1);

        // connect to the requestor's receive port
        workSender.connect(requestor);

        Cube cube = getFromPool(false);
        if(cube != null) {
            syncTermination.increaseBusyWorkers();
        }
        // create a reply message
        WriteMessage reply = workSender.newMessage();
        reply.writeObject(cube);
        reply.finish();
        workSender.close();
    }

    //method called by Slaves to getWork, if the work queue is empty, some work is asked to the server
    //if this method return null, that means that there is no more work to do
    public static Cube getWork() throws IOException, ClassNotFoundException {
        Cube cube;
        cube = getFromPool(true);
        if(cube == null) {
            cube = askForWork();
        }

        return cube;
    }


    //send the actual results, and as response receive if another bound has to be evaluated
    //or if the system can terminate
    public static boolean sendResults(int res) {
        boolean termination;
        //send local work receiving port
        WriteMessage resMsg = resultsSender.newMessage();
        resMsg.writeInt(res);
        resMsg.finish();

        //get the work
        ReadMessage r = terminationReceiver.receive();
        termination = r.readBoolean();
        r.finish();
        return termination;
    }

    public static class ResultsUpdater implements MessageUpcall {

        public void upcall(ReadMessage message) throws IOException,
            ClassNotFoundException {
            int results = message.readInt();
            syncTermination.increaseResults(results);

        }


    }

    public void solutionsWorkers() {
        Cube cube = null;
        CubeCache cache;
        boolean first = true;
        int results = 0;
        boolean end = false;

        //while there are bounds to evaluate
        while(!end) {
            //while there is work for the actual bound
            while((cube = getWork()) != null) {
                //cache initialization with rhe first received cube
                if(first) {
                    cache = new CubeCache(cube.getSize());
                    first = false;
                }
                results += solution(cube, cache);
            }
            end = sendResults(result);
        }
    }

    public void solutionsServer(CubeCache cache, SyncTermination syncTermination) {
        //increase the number of ibis workes (at least me)
        syncTermination.increaseBusyWorkers();
        int results = 0;
        Cube cube;

        //while the work pool is not empty, continue to work
        while((cube = getFromPool(true)) != null) {
            synchronized(lock) {
                cube = toDo.remove(toDo.size() - 1);
            }
            if(cube != initialCube) {
                cache.add(cube);
            }
            results += solution(cube, cache);
        }

        //add my results to the cumulative results
        syncTermination.increaseResults(results);

        //wait until all the slaves terminate the calculation for this bound and get the cumulative results
        int boundResult = syncTermination.waitTermination();
        return boundResult;
    }


    public void solveServer(Ibis ibis) {
        ResultsUpdater resultsUpdater = new ResultsUpdater();
        //port in which new work requests will be received
        workRequestReceiver = ibis.createReceivePort(portTypeMto1Up, "WorkReq", this);
        // enable connections
        workRequestReceiver.enableConnections();
        // enable upcalls
        workRequestReceiver.enableMessageUpcalls();

        resultsReceiver = ibis.createReceivePort(portTypeMto1Up, "Results", resultsUpdater);
        resultsReceiver.enableConnections();

        terminationSender = ibis.createSendPort(portTypeMto1);
        //connect with every receive port*/

        Cube cube = generateCube();
        CubeCache cubeCache = new CubeCache(cube.getSize());

        syncTermination = new SyncTermination();

        int bound = 0;
        int result = 0;

        for (IbisIdentifier joinedIbis : joinedIbises) {
            if(joinedIbis.equals(myIbisId)) {
                continue;
            }
            terminationSender.connect(joinedIbis, "Termination");
        }

        WriteMessage termination;


        System.out.print("Bound now:");

        long start = System.currentTimeMillis();
        while (result == 0) {
            bound++;
            cube.setBound(bound);
            System.out.print(" " + bound);
            result = solutionsServer(cache, syncTermination);
            if(result == 0) {
                termination = terminationSender.newMessage();
                termination.writeBoolean(false);
                termination.finish();
            }
        }
        long end = System.currentTimeMillis();
        termination = terminationSender.newMessage();
        termination.writeBoolean(true);
        termination.finish();

        System.out.println();
        System.out.println("Solving cube possible in " + result + " ways of "
                           + bound + " steps");

        System.err.println("Solving cube took " + (end - start)
                           + " milliseconds");

        //close all ports
        terminationSender.close();
        Thread.sleep(1000);
        resultsReceiver.close();
        workRequestReceiver.close();
    }

    public void solveWorkers(Ibis ibis) {
        //workReceiver = ibis.createReceivePort(portType1to1, "Work");
        workReceiver = ibis.createReceivePort(portType1to1, "Work");
        workReceiver.enableConnections();

        terminationReceiver = ibis.createReceivePort(portTypeMto1, "Termination");
        terminationReceiver.enableConnections();

        //port in which new work requests will be sent
        workRequestSender = ibis.createSendPort(portTypeMto1Up);
        workRequestSender.connect(server, "WorkReq");

        resultsSender = ibis.createSendPort(portTypeMto1Up);
        resultsSender.connect(server, "Results");

        solutionsWorkers();

        //close all the ports
        workRequestSender.close();
        workRequestSender.close();
        Thread.sleep(1000);
        workReceiver.close();
        terminationReceiver.close();



    }

    public static Cube generateCube() {
        Cube cube = null;

        // default parameters of puzzle
        int size = 3;
        int twists = 11;
        int seed = 0;
        String fileName = null;

        // number of threads used to solve puzzle
        // (not used in sequential version)

        for (int i = 0; i < arguments.length; i++) {
            if (arguments[i].equalsIgnoreCase("--size")) {
                i++;
                size = Integer.parseInt(arguments[i]);
            } else if (arguments[i].equalsIgnoreCase("--twists")) {
                i++;
                twists = Integer.parseInt(arguments[i]);
            } else if (arguments[i].equalsIgnoreCase("--seed")) {
                i++;
                seed = Integer.parseInt(arguments[i]);
            } else if (arguments[i].equalsIgnoreCase("--file")) {
                i++;
                fileName = arguments[i];
            } else if (arguments[i].equalsIgnoreCase("--help") || arguments[i].equalsIgnoreCase("-h")) {
                printUsage();
                System.exit(0);
            } else {
                System.err.println("unknown option : " + arguments[i]);
                printUsage();
                System.exit(1);
            }
        }

        // create cube
        if (fileName == null) {
            cube = new Cube(size, twists, seed);
        } else {
            try {
                cube = new Cube(fileName);
            } catch (Exception e) {
                System.err.println("Cannot load cube from file: " + e);
                System.exit(1);
            }
        }

        // print cube info
        System.out.println("Searching for solution for cube of size "
                           + cube.getSize() + ", twists = " + twists + ", seed = " + seed);
        cube.print(System.out);
        System.out.flush();
        return cube;
    }

    private void run() throws Exception {
        //System.out.println("done");
        // Create an ibis instance.
        Ibis ibis = IbisFactory.createIbis(ibisCapabilities, null, portTypeMto1Up, portType1to1);
        Thread.sleep(5000);
        System.out.println("Ibis created");
        myIbisId = ibis.identifier();
        myIbis = ibis;

        // Elect a server
        System.out.println("elections");
        server = ibis.registry().elect("Server");

        System.out.println("Server is " + server);

        joinedIbises = ibis.registry().joinedIbises();
        nodes = joinedIbises.length;


        // If I am the server, run server, else run client.
        if (server.equals(ibis.identifier())) {
            if(initialCube == null) {
                System.out.println("CUBE NULL FROM THE BEGIN");
            } else {
                System.out.println("CUBE ok");
            }
            solveServer(ibis);


            //long end = System.currentTimeMillis();

            // NOTE: this is printed to standard error! The rest of the output is
            // constant for each set of parameters. Printing this to standard error
            // makes the output of standard out comparable with "diff"

            //terminate all workers
            // terminate the pool
            //System.out.println("Terminating pool");
            //ibis.registry().terminate();
            // wait for this termination to propagate through the system
            //ibis.registry().waitUntilTerminated();


        } else {
            solveWorkers(ibis);
        }

        workRequestSender.close();
        Thread.sleep(1000);
        workRequestReceiver.close();
        workReceiver.close();
        ibis.end();
    }


    public static void main(String[] arguments) {
        this.arguments = arguments;
        try {
            System.out.println("run");
            new Rubiks().run();
        } catch (Exception e) {
            e.printStackTrace(System.err);
        }
    }
}

