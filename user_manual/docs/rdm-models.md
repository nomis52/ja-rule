RDM Models  {#rdm-models}
==========================

The Ja Rule code can simulate different types of RDM Responder Models. This
allows RDM controllers to be well tested, without having to go out and buy a
lot of different gear.

RDM can be used to select which model to use, two manufacturer PIDs are
provided:
[MODEL_ID_LIST](http://rdm.openlighting.org/pid/display?manufacturer=31344&pid=32771)
&
[MODEL_ID](http://rdm.openlighting.org/pid/display?manufacturer=31344&pid=32770)
Note that these PIDs do not appear in the supported parameters list, so they
are 'hidden' in a sense.

Using the OLA CLI, to get a list of available models:
~~~~~
$ ola_rdm_get -u 1 --uid 7a70:fffffe00 model_id_list
~~~~~

To change the current model:
~~~~~
$ ola_rdm_set -u 1 --uid 7a70:fffffe00 model_id 258
~~~~~
